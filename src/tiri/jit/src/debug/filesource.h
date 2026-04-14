// FileSource tracking for accurate error reporting in imported files.
// Copyright © 2025-2026 Paul Manias

#pragma once

#include <string>
#include <optional>
#include <cstdint>
#include "lj_obj.h"

// FileSource tracks source file metadata for accurate error reporting when code is imported.
// Each file gets assigned a unique index (0-254), with index 0 reserved for the main file.
// Index 255 is reserved as an overflow fallback when the file limit is exceeded.

struct FileSource {
   std::string path;               // Full resolved path
   std::string filename;           // Short name for error display
   std::string declared_namespace; // From 'namespace' statement
   BCLine first_line;              // First line in unified space (for reconstruction)
   BCLine total_lines;             // Total lines in source file
   uint32_t path_hash;             // For fast deduplication lookup
   uint8_t parent_file_index;      // Which file imported this one (0 for main)
   BCLine import_line;             // Line in parent where import occurred (0 for main)
};

// Maximum file index (255 is reserved for overflow)
constexpr uint8_t FILESOURCE_MAX_INDEX = 254;
constexpr uint8_t FILESOURCE_OVERFLOW_INDEX = 255;

// Register a new file source in the lua_State.
// Returns the file index, or FILESOURCE_OVERFLOW_INDEX (255) if the limit is exceeded.
// The overflow index is initialised with "unknown" on first use.
// Path will be resolved to an absolute path if possible.
uint8_t register_file_source(lua_State *L, std::string &Path, const std::string &Filename,
   BCLine FirstLine, BCLine SourceLines, uint8_t ParentIndex, BCLine ImportLine);

// Find a file source by path hash.
// Returns the file index if found, or std::nullopt if not found.
std::optional<uint8_t> find_file_source(lua_State *L, uint32_t PathHash);

// Get a file source by index.
// Returns nullptr if the index is out of range.
const FileSource* get_file_source(lua_State *L, uint8_t Index);

// Check if an index represents the overflow fallback.
[[nodiscard]] inline bool is_file_source_overflow(uint8_t Index) noexcept
{
   return Index IS FILESOURCE_OVERFLOW_INDEX;
}

// Register a file being parsed as a "main" file source (from lj_parse or loadFile).
// Returns the file index (may be > 0 if this file was already registered or other files exist).
uint8_t register_main_file_source(lua_State *, std::string &, const std::string &, BCLine);

// Set the declared namespace for a file source.
// Returns true if successful, false if the index is out of range.
bool set_file_source_namespace(lua_State *, uint8_t, const std::string &);

// Find a file source by its declared namespace.
// Returns the file index if found, or std::nullopt if not found.
std::optional<uint8_t> find_file_source_by_namespace(lua_State *, const std::string &);

int widest_file_source(lua_State *, bool);
