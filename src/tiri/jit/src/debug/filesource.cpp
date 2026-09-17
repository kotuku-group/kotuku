// FileSource tracking for accurate error reporting in imported files.
//
// Copyright © 2025-2026 Paul Manias

#include "filesource.h"
#include "../runtime/lj_gc.h"
#include "../runtime/lj_str.h"
#include "../lib/load.h"
#include <kotuku/main.h>
#include <array>
#include <unordered_set>

//********************************************************************************************************************
// Register a new file source in the lua_State.
// Returns the file index, or FILESOURCE_OVERFLOW_INDEX (255) if the limit is exceeded.
// NB: Path is the full path, including filename.

uint8_t register_file_source(lua_State *L, std::string &Path, const std::string &Filename, BCLine FirstLine,
   BCLine SourceLines, uint8_t ParentIndex, BCLine ImportLine, bool ProvisionalCacheMetadata)
{
   kt::Log log(__FUNCTION__);

   if (L->file_sources.size() >= FILESOURCE_MAX_COUNT) {
      log.msg("FileSource limit exceeded (%d files). Additional imports will show as 'unknown'.", FILESOURCE_MAX_COUNT);
      return FILESOURCE_OVERFLOW_INDEX;
   }

   if (not Path.starts_with("scripts:")) {
      std::string resolved_path;
      if (ResolvePath(Path, RSF::NO_FILE_CHECK, &resolved_path) IS ERR::Okay) Path = resolved_path;
   }

   auto path_hash = kt::strihash(Path);

   // Check if this file is already registered

   if (auto existing = find_file_source(L, Path)) {
      log.msg("File already registered: %s $%.8x (index %d)", Filename.c_str(), path_hash, *existing);
      return *existing;
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
   source.provisional_cache_metadata = ProvisionalCacheMetadata;

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

std::optional<uint8_t> find_file_source(lua_State *L, const std::string &Path)
{
   const uint32_t hash = kt::strihash(Path);
   if (auto index = find_file_source(L, hash);
       index and *index < L->file_sources.size() and L->file_sources[*index].path IS Path) return index;
   for (size_t i = 0; i < L->file_sources.size(); ++i) {
      if (L->file_sources[i].path IS Path) return uint8_t(i);
   }
   return std::nullopt;
}

//********************************************************************************************************************

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

//********************************************************************************************************************

void attach_compilation_source_root(GCproto *Prototype, GCproto *Root)
{
   if (Prototype and Root) set_source_root(Prototype, Root);
}

//********************************************************************************************************************

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
      setgcref(entries[i].canonical_path, obj2gco(lj_str_new(L, source.canonical_path.data(), source.canonical_path.size())));
      setgcref(entries[i].display_filename, obj2gco(lj_str_new(L, source.display_filename.data(), source.display_filename.size())));
      setgcref(entries[i].declared_namespace, obj2gco(lj_str_new(L, source.declared_namespace.data(), source.declared_namespace.size())));
      entries[i].first_line = source.first_line;
      entries[i].total_lines = source.total_lines;
      entries[i].import_line = source.import_line;
      entries[i].runtime_index = source.runtime_index;
      entries[i].parent = source.parent;
      entries[i].role = source.role;
      entries[i].reserved = 0;
   }
}

//********************************************************************************************************************

static uint8_t loaded_source_id(uint8_t Wire, const std::vector<CompilationSourceRecord> &Records)
{
   if (Wire < Records.size()) return Records[Wire].runtime_index;
   return Wire;
}

//********************************************************************************************************************

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

//********************************************************************************************************************

void attach_loaded_compilation_sources(lua_State *L, GCproto *Root,
   const std::vector<CompilationSourceRecord> &Records, std::span<GCproto *const> AdditionalRoots,
   BytecodeLoadOperationCounters *Operations)
{
   kt::Log log(__FUNCTION__);
   std::vector<CompilationSourceRecord> resolved = Records;
   L->file_sources.reserve(std::min<size_t>(FILESOURCE_MAX_COUNT, L->file_sources.size() + Records.size()));
   L->file_index_map.reserve(L->file_sources.size() + Records.size());

   for (size_t i = 0; i < resolved.size(); ++i) {
      auto &source = resolved[i];
      if (source.role IS CompilationSourceRole::Synthetic) {
         source.runtime_index = FILESOURCE_SYNTHETIC_INDEX;
         continue;
      }

      const std::optional<uint8_t> existing = find_file_source(L, source.canonical_path);

      if (existing) {
         source.runtime_index = existing.value();
         FileSource &registered = L->file_sources[source.runtime_index];

         // The parser pre-registers warm graph sources from portable interfaces before installing the payload.  Only
         // those provisional entries may be enriched: an older registration can still be referenced by live code.

         if (registered.provisional_cache_metadata) {
            if (i and source.parent < i) {
               registered.parent_file_index = resolved[source.parent].runtime_index;
               registered.import_line = source.import_line;
            }
            registered.first_line         = source.first_line;
            registered.total_lines        = source.total_lines;
            registered.filename           = source.display_filename;
            registered.declared_namespace = source.declared_namespace;
            registered.provisional_cache_metadata = false;
         }
         else if (registered.filename != source.display_filename or registered.first_line != source.first_line or
                  registered.total_lines != source.total_lines or
                  registered.declared_namespace != source.declared_namespace) {
            log.warning("BC metadata for '%s' conflicts with the active registration.  Clear 'temp:tiri/cache/' if the bytecode came from the automatic cache.",
               source.canonical_path.c_str());
         }
      }
      else {
         uint8_t parent = source.parent < i ? resolved[source.parent].runtime_index : 0;
         std::string path = source.canonical_path;
         source.runtime_index = register_file_source(L, path, source.display_filename, source.first_line,
            source.total_lines, parent, source.import_line);
         if (Operations) Operations->file_source_registrations++;
         set_file_source_namespace(L, source.runtime_index, source.declared_namespace);
      }
   }

   remap_loaded_sources(Root, resolved);
   if (Operations) Operations->line_map_remaps++;
   for (GCproto *prototype : AdditionalRoots) remap_loaded_sources(prototype, resolved);
   attach_compilation_sources(L, Root, resolved);
   if (Operations) Operations->source_map_allocations++;
   for (GCproto *prototype : AdditionalRoots) set_source_root(prototype, Root);
}

//********************************************************************************************************************
// Collect every runtime source referenced by one prototype tree without changing its bytecode or line information.

static bool collect_prototype_sources(GCproto *Root, std::array<bool, 256> &Required,
   std::unordered_set<GCproto *> &Visited)
{
   if (not Root or not Visited.insert(Root).second) return Root != nullptr;

   Required[Root->file_source_idx] = true;
   if (const BCLine *lines = (const BCLine *)proto_lineinfo(Root)) {
      for (MSize i = 0; i + 1 < Root->sizebc; ++i) Required[lines[i].fileIndex()] = true;
   }

   if (Root->flags & PROTO_CHILD) {
      GCRef *constant = mref<GCRef>(Root->k) - 1;
      for (MSize i = 0; i < Root->sizekgc; ++i, --constant) {
         GCobj *object = gcref(*constant);
         if (object->gch.gct IS ~LJ_TPROTO and
             not collect_prototype_sources(gco_to_proto(object), Required, Visited)) return false;
      }
   }

   return true;
}

//********************************************************************************************************************
// Prepare a dense standalone source map.  Sources outside the standalone root's ancestry are attached directly to
// that root, avoiding stale producer-application records while retaining deterministic original compilation order.

bool prepare_standalone_compilation_sources(std::span<const CompilationSourceRecord> Sources, GCproto *Root,
   std::span<GCproto *const> AdditionalRoots, std::span<const uint8_t> ModuleSources,
   PreparedCompilationSources &Result)
{
   Result = PreparedCompilationSources {};
   if (not Root or Sources.empty() or Sources.size() > FILESOURCE_MAX_COUNT) return false;

   std::array<int16_t, 256> runtime_to_compilation;
   runtime_to_compilation.fill(-1);

   for (size_t i = 0; i < Sources.size(); ++i) {
      const uint8_t runtime = Sources[i].runtime_index;
      if (runtime IS FILESOURCE_OVERFLOW_INDEX) continue;
      if (runtime_to_compilation[runtime] >= 0) return false;
      runtime_to_compilation[runtime] = int16_t(i);
   }

   const int16_t root_compilation = runtime_to_compilation[Root->file_source_idx];
   if (root_compilation < 0) return false;

   std::array<bool, 256> required{};
   std::unordered_set<GCproto *> visited;

   if (not collect_prototype_sources(Root, required, visited)) return false;
   for (GCproto *prototype : AdditionalRoots) {
      if (not collect_prototype_sources(prototype, required, visited)) return false;
   }

   for (uint8_t source : ModuleSources) {
      if (source != FILESOURCE_OVERFLOW_INDEX) required[source] = true;
   }

   if (required[FILESOURCE_SYNTHETIC_INDEX] and runtime_to_compilation[FILESOURCE_SYNTHETIC_INDEX] < 0) return false;
   required[FILESOURCE_OVERFLOW_INDEX] = false;

   std::vector<bool> selected(Sources.size());
   selected[size_t(root_compilation)] = true;
   for (size_t runtime = 0; runtime < required.size(); ++runtime) {
      if (not required[runtime] or runtime IS FILESOURCE_SYNTHETIC_INDEX) continue;
      const int16_t compilation = runtime_to_compilation[runtime];
      if (compilation < 0) return false;
      selected[size_t(compilation)] = true;
   }

   Result.compilation_to_wire.assign(Sources.size(), FILESOURCE_OVERFLOW_INDEX);
   auto append = [&](size_t Compilation, bool IsRoot) {
      CompilationSourceRecord record = Sources[Compilation];
      const uint8_t wire = uint8_t(Result.records.size());
      Result.compilation_to_wire[Compilation] = wire;
      if (record.runtime_index != FILESOURCE_OVERFLOW_INDEX) Result.runtime_to_wire[record.runtime_index] = wire;
      if (IsRoot) {
         record.role = record.role IS CompilationSourceRole::Synthetic ?
            CompilationSourceRole::Synthetic : CompilationSourceRole::Main;
         record.parent = FILESOURCE_OVERFLOW_INDEX;
         record.import_line = 0;
      }
      else {
         const uint8_t parent = record.parent;

         if (parent < Result.compilation_to_wire.size() and
             Result.compilation_to_wire[parent] != FILESOURCE_OVERFLOW_INDEX) {
            record.parent = Result.compilation_to_wire[parent];
         }
         else {
            record.parent = 0;
            if (record.import_line.lineNumber() IS 0 or
                record.import_line.lineNumber() > Result.records[0].total_lines.lineNumber()) record.import_line = 1;
         }

         record.role = CompilationSourceRole::Import;
      }
      Result.records.push_back(std::move(record));
   };

   append(size_t(root_compilation), true);
   for (size_t i = 0; i < Sources.size(); ++i) {
      if (i != size_t(root_compilation) and selected[i]) append(i, false);
   }

   return not Result.records.empty();
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
