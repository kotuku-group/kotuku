// A Kotuku friendly version of the Lua's 'io' library.  Provided mostly for compatibility purposes, but also makes
// it easier to access the std* file handles.

#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/tiri.h>
#include <kotuku/strings.hpp>
#include <inttypes.h>

#include "lauxlib.h"
#include "lj_obj.h"
#include "hashes.h"
#include "defs.h"
#include "lj_proto_registry.h"

enum {
   CONST_STDIN  = -1,
   CONST_STDOUT = -2,
   CONST_STDERR = -3
};

// File handle userdata structure.  Uses file identifiers exclusively so that we don't have to be concerned about
// invalid pointers.

struct FileHandle {
   OBJECTID file_id = 0;
   bool auto_close = true;

   FileHandle(objFile *f, bool ac = true) : file_id(f->UID), auto_close(ac) {}

   // Close the file handle and optionally free the underlying file object.
   // Returns true if the file was actually closed.

   bool close() {
      if (file_id) {
         if (auto_close) {
            if (auto fl = (objFile *)GetObjectPtr(file_id)) FreeResource(fl);
         }
         file_id = 0;
         return true;
      }
      return false;
   }
};

// Helper functions

inline FileHandle * check_file_handle(lua_State *Lua, int Index)
{
   return (FileHandle *)luaL_checkudata(Lua, Index, "Tiri.file");
}

inline int push_file_handle(lua_State *Lua, objFile *File, bool AutoClose = true)
{
   auto handle = (FileHandle *)lua_newuserdata(Lua, sizeof(FileHandle));
   new(handle) FileHandle(File, AutoClose);
   luaL_getmetatable(Lua, "Tiri.file");
   lua_setmetatable(Lua, -2);
   return 1;
}

inline int file_gc(lua_State *Lua)
{
   if (auto handle = check_file_handle(Lua, 1)) {
      handle->close();
   }
   return 0;
}

//********************************************************************************************************************
// Forward declarations

static int io_input(lua_State *);
static int io_output(lua_State *);

static int file_read(lua_State *);
static int file_write(lua_State *);
static int file_flush(lua_State *);

static int io_readAll(lua_State *);
static int io_writeAll(lua_State *);
static int io_isFolder(lua_State *);
static int io_splitPath(lua_State *);
static int io_sanitisePath(lua_State *);

//********************************************************************************************************************
// Usage: file = io.open(path, [mode])

static int io_open(lua_State *Lua)
{
   auto path = luaL_checkstring(Lua, 1);
   auto mode = luaL_optstring(Lua, 2, "r");
   auto seek_end = false;
   auto flags = FL::NIL;

   for (int i = 0; mode[i]; i++) {
      switch (mode[i]) {
         case 'r': flags |= FL::READ; break;
         case 'w': flags |= FL::WRITE | FL::NEW; break;
         case 'a':
            if (AnalysePath(path, nullptr) IS ERR::Okay) flags |= FL::WRITE;
            else flags |= FL::WRITE | FL::NEW;
            seek_end = true;
            break;  // Append mode - will seek to end after open
         case '+': flags |= FL::READ | FL::WRITE; break;
      }
   }

   if (auto file = objFile::create::local({ fl::Path(path), fl::Flags(flags) })) {
      if (seek_end) file->seekEnd(0);

      push_file_handle(Lua, file);
      return 1;
   }
   else {
      luaL_error(Lua, ERR::OpenFile);
      return 0;
   }
}

//********************************************************************************************************************
// Usage: io.close([file])

static int io_close(lua_State *Lua)
{
   if (lua_gettop(Lua) IS 0) {
      // TODO: Close default output file with FreeResource() and remove it from the registry
      return 0;
   }

   if (auto handle = check_file_handle(Lua, 1)) {
      handle->close();
      lua_pushboolean(Lua, 1);
      return 1;
   }

   return 0;
}

//********************************************************************************************************************
// Usage: data1, data2, ... = io.read(...)

static int io_read(lua_State *Lua)
{
   // Get default input file
   lua_pushstring(Lua, "io.defaultInput");
   lua_gettable(Lua, LUA_REGISTRYINDEX);

   if (lua_isnil(Lua, -1)) {
      // Initialize default input by calling io.input()
      lua_pop(Lua, 1);  // Remove nil
      lua_pushcfunction(Lua, io_input);
      lua_call(Lua, 0, 1);  // Call io.input() with no args
   }

   if (lua_isnil(Lua, -1)) luaL_error(Lua, ERR::ExpectedFile);

   // Insert the file handle as first argument and call file:read
   lua_insert(Lua, 1);
   return file_read(Lua);
}

//********************************************************************************************************************
// Usage: io.write(data1, data2, ...)
//
// Forwards the parameters to the default output file's write() method.

static int io_write(lua_State *Lua)
{
   // Get default output file
   lua_pushstring(Lua, "io.defaultOutput");
   lua_gettable(Lua, LUA_REGISTRYINDEX);

   if (lua_isnil(Lua, -1)) {
      // Initialize default output by calling io.output()
      lua_pop(Lua, 1);  // Remove nil
      lua_pushcfunction(Lua, io_output);
      lua_call(Lua, 0, 1);  // Call io.output() with no args
   }

   if (lua_isnil(Lua, -1)) luaL_error(Lua, ERR::ExpectedFile);

   // Insert the file handle as first argument and call file:write
   lua_insert(Lua, 1);
   return file_write(Lua);
}

//********************************************************************************************************************
// Usage: success = io.flush()

static int io_flush(lua_State *Lua)
{
   // Get default output file
   lua_pushstring(Lua, "io.defaultOutput");
   lua_gettable(Lua, LUA_REGISTRYINDEX);

   if (lua_isnil(Lua, -1)) {
      // Initialize default output by calling io.output()
      lua_pop(Lua, 1);  // Remove nil
      lua_pushcfunction(Lua, io_output);
      lua_call(Lua, 0, 1);  // Call io.output() with no args
   }

   if (lua_isnil(Lua, -1)) luaL_error(Lua, ERR::File);

   // Call file:flush on the default output
   return file_flush(Lua);
}

//********************************************************************************************************************
// Usage: file = io.input([file|string|fd])

static int io_input(lua_State *Lua)
{
   if (lua_gettop(Lua) IS 0) {
      // Return current default input
      lua_pushstring(Lua, "io.defaultInput");
      lua_gettable(Lua, LUA_REGISTRYINDEX);

      if (lua_isnil(Lua, -1)) { // No default set, try to open stdin
         lua_pop(Lua, 1);  // Remove nil

         auto file = objFile::create::local({ fl::Path("std:in"), fl::Flags(FL::READ) });

         if (file) {
            push_file_handle(Lua, file, false);  // Don't auto-close stdin

            // Store as default
            lua_pushstring(Lua, "io.defaultInput");
            lua_pushvalue(Lua, -2);  // Copy the file handle
            lua_settable(Lua, LUA_REGISTRYINDEX);
         }
         else luaL_error(Lua, ERR::OpenFile);
      }
      return 1;
   }

   // Set new default input
   if ((lua_type(Lua, 1) IS LUA_TSTRING) or (lua_type(Lua, 1) IS LUA_TNUMBER)) {
      std::string path;
      if (lua_type(Lua, 1) IS LUA_TNUMBER) {
         switch(lua_tointeger(Lua, 1)) {
            case CONST_STDIN: path = "std:in"; break;
            default: luaL_error(Lua, ERR::File, "Invalid file descriptor");
         }
      }
      else path = lua_tostring(Lua, 1);

      auto file = objFile::create::local({ fl::Path(path), fl::Flags(FL::READ) });

      if (file) {
         push_file_handle(Lua, file);

         // Store as default
         lua_pushstring(Lua, "io.defaultInput");
         lua_pushvalue(Lua, -2);  // Copy the file handle
         lua_settable(Lua, LUA_REGISTRYINDEX);

         return 1;
      }
      else luaL_error(Lua, ERR::OpenFile);
   }
   else if (check_file_handle(Lua, 1)) { // Use provided file handle as the new default
      lua_pushstring(Lua, "io.defaultInput");
      lua_pushvalue(Lua, 1);  // Copy the file handle
      lua_settable(Lua, LUA_REGISTRYINDEX);
      lua_pushvalue(Lua, 1);  // Return the file handle
      return 1;
   }
   else luaL_argerror(Lua, 1, "Invalid argument, expected string or file handle");

   return 0;
}

//********************************************************************************************************************
// Usage: file = io.output([file|string])

static int io_output(lua_State *Lua)
{
   if (lua_gettop(Lua) IS 0) {
      // Return current default output
      lua_pushstring(Lua, "io.defaultOutput");
      lua_gettable(Lua, LUA_REGISTRYINDEX);
      if (lua_isnil(Lua, -1)) {
         // No default set, try to open stdout
         lua_pop(Lua, 1);  // Remove nil

         if (auto file = objFile::create::local({ fl::Path("std:out"), fl::Flags(FL::WRITE) })) {
            push_file_handle(Lua, file, false);  // Don't auto-close stdout

            // Store as default
            lua_pushstring(Lua, "io.defaultOutput");
            lua_pushvalue(Lua, -2);  // Copy the file handle
            lua_settable(Lua, LUA_REGISTRYINDEX);
         }
         else luaL_error(Lua, ERR::OpenFile);
      }
      return 1;
   }

   // Set new default output

   if (lua_type(Lua, 1) IS LUA_TSTRING) { // Open file for writing
      auto path = lua_tostring(Lua, 1);

      if (auto file = objFile::create::local({ fl::Path(path), fl::Flags(FL::NEW|FL::WRITE) })) {
         push_file_handle(Lua, file);

         // Store as default
         lua_pushstring(Lua, "io.defaultOutput");
         lua_pushvalue(Lua, -2);  // Copy the file handle
         lua_settable(Lua, LUA_REGISTRYINDEX);
         return 1;
      }
      else luaL_error(Lua, ERR::OpenFile);
   }
   else if (check_file_handle(Lua, 1)) { // Use provided file handle
      // Store as default
      lua_pushstring(Lua, "io.defaultOutput");
      lua_pushvalue(Lua, 1);  // Copy the file handle
      lua_settable(Lua, LUA_REGISTRYINDEX);
      lua_pushvalue(Lua, 1);  // Return the file handle
      return 1;
   }
   else luaL_argerror(Lua, 1, "Invalid argument, expected string or file handle");

   return 0;
}

//********************************************************************************************************************

// Iterator state for io.lines
struct LinesIterator {
   FileHandle *file_handle = nullptr;
   bool close_on_finish;

   LinesIterator(FileHandle *fh, bool close) : file_handle(fh), close_on_finish(close) {}
};

static int lines_iterator(lua_State *Lua)
{
   auto iter = (LinesIterator *)lua_touserdata(Lua, lua_upvalueindex(1));

   if (not iter->file_handle) return 0; // End iteration

   auto file = (objFile *)GetObjectPtr(iter->file_handle->file_id);

   if (not file) return 0; // End iteration

   struct fl::ReadLine args;
   if (Action(fl::ReadLine::id, file, &args) IS ERR::Okay) {
      lua_pushstring(Lua, args.Result);
      return 1;
   }
   else { // End of file or error - close if we own it
      if (iter->close_on_finish) {
         iter->file_handle->close();
         iter->file_handle = nullptr;
      }
      return 0; // End iteration
   }
}

static int lines_iterator_gc(lua_State *Lua)
{
   auto iter = (LinesIterator *)lua_touserdata(Lua, 1);
   if (iter->close_on_finish and iter->file_handle) {
      iter->file_handle->close();
      iter->file_handle = nullptr;
   }
   return 0;
}

static int io_lines(lua_State *Lua)
{
   FileHandle *file_handle = nullptr;
   bool close_on_finish = false;

   if (lua_gettop(Lua) IS 0) {
      // No arguments - use default input
      lua_pushstring(Lua, "io.defaultInput");
      lua_gettable(Lua, LUA_REGISTRYINDEX);

      if (lua_isnil(Lua, -1)) {
         // Initialize default input
         lua_pop(Lua, 1);
         lua_pushcfunction(Lua, io_input);
         lua_call(Lua, 0, 1);
      }

      if (lua_isnil(Lua, -1)) {
         luaL_error(Lua, ERR::InvalidState, "No default input file available");
         return 0;
      }

      file_handle = check_file_handle(Lua, -1);
      close_on_finish = false; // Don't close default input
   }
   else if (lua_type(Lua, 1) IS LUA_TSTRING) { // Filename provided - open file
      auto path = lua_tostring(Lua, 1);

      if (auto file = objFile::create::local({ fl::Path(path), fl::Flags(FL::READ) })) {
         push_file_handle(Lua, file);
         file_handle = check_file_handle(Lua, -1);
         close_on_finish = true; // Close when iteration ends
      }
      else luaL_error(Lua, ERR::File, "Cannot open file: %s", path);
   }
   else {
      // File handle provided
      file_handle = check_file_handle(Lua, 1);
      close_on_finish = false; // Don't close provided handle - we don't own it
   }

   if (file_handle) {
      // Create iterator state
      auto iter = (LinesIterator *)lua_newuserdata(Lua, sizeof(LinesIterator));
      new (iter) LinesIterator(file_handle, close_on_finish);

      // Set up GC metamethod for iterator state
      lua_newtable(Lua);
      lua_pushcfunction(Lua, lines_iterator_gc);
      lua_setfield(Lua, -2, "__gc");
      lua_setmetatable(Lua, -2);

      // Return the iterator function with the state as upvalue
      lua_pushcclosure(Lua, lines_iterator, 1);
      return 1;
   }
   else luaL_error(Lua, ERR::File);
}

//********************************************************************************************************************
// TODO: Open pipe to running process - requires Task integration and using callbacks to receive data from stdout.
// This is equivalent to Lua's io.popen()

static int io_openPipe(lua_State *Lua)
{
   luaL_error(Lua, ERR::NoSupport, "io.openPipe not yet implemented");
   return 0;
}

//********************************************************************************************************************
// Create a temporary buffer file in memory.  In theory this is the best and most performant option if you
// also consider that the OS can use swap space for large memory files.

static int io_tempFile(lua_State *Lua)
{
   if (auto file = objFile::create::local({ fl::Size(4096), fl::Flags(FL::BUFFER|FL::READ|FL::WRITE) })) {
      push_file_handle(Lua, file);
      return 1;
   }
   else luaL_error(Lua, ERR::CreateFile);
}

//********************************************************************************************************************

static int io_type(lua_State *Lua)
{
   if (lua_type(Lua, 1) IS LUA_TUSERDATA) {
      auto handle = (FileHandle *)luaL_testudata(Lua, 1, "Tiri.file");
      if (handle) {
         if (handle->file_id) lua_pushstring(Lua, "file");
         else lua_pushstring(Lua, "closed file");
         return 1;
      }
   }

   lua_pushnil(Lua);
   return 1;
}

//********************************************************************************************************************
// File handle methods

static int file_read(lua_State *Lua)
{
   if (auto handle = check_file_handle(Lua, 1)) {
      if (not handle->file_id) luaL_error(Lua, ERR::InvalidState, "Attempted to use a closed file");

      auto file = (objFile *)GetObjectPtr(handle->file_id);
      if (not file) luaL_error(Lua, ERR::InvalidState, "Attempted to use an orphaned file handle");

      int nargs = lua_gettop(Lua);

      // Default to reading a line if no arguments
      if (nargs IS 1) {
         struct fl::ReadLine args;
         if (Action(fl::ReadLine::id, file, &args) IS ERR::Okay) {
            lua_pushstring(Lua, args.Result);
            return 1;
         }
         else {
            lua_pushnil(Lua);
            return 1;
         }
      }

      // Process read format arguments
      for (int i = 2; i <= nargs; i++) {
         if (lua_type(Lua, i) IS LUA_TSTRING) {
            auto format = lua_tostring(Lua, i);

            if (format[0] IS '*') {
               switch (format[1]) {
                  case 'n': { // Read a number
                     struct fl::ReadLine args;
                     if (Action(fl::ReadLine::id, file, &args) IS ERR::Okay) {
                        lua_pushnumber(Lua, std::strtod(args.Result, nullptr));
                     }
                     else lua_pushnil(Lua);
                     break;
                  }

                  case 'a': { // Read entire file
                     auto current_pos = file->Position;
                     file->seekEnd(0);
                     auto file_size = file->Position;
                     file->seek(current_pos, SEEK::START);

                     auto remaining = file_size - current_pos;
                     if (remaining > 0) {
                        std::string buffer(remaining, '\0');
                        int bytes_read;
                        if (acRead(file, buffer.data(), remaining, &bytes_read) IS ERR::Okay) {
                           buffer.resize(bytes_read);
                           lua_pushlstring(Lua, buffer.data(), bytes_read);
                        }
                        else lua_pushnil(Lua);
                     }
                     else lua_pushstring(Lua, "");
                     break;
                  }

                  case 'l': { // Read a line (default behavior)
                     struct fl::ReadLine args;
                     if (Action(fl::ReadLine::id, file, &args) IS ERR::Okay) {
                        lua_pushstring(Lua, args.Result);
                     }
                     else lua_pushnil(Lua);
                     break;
                  }

                  default:
                     lua_pushnil(Lua);
                     break;
               }
            }
            else lua_pushnil(Lua);
         }
         else if (lua_type(Lua, i) IS LUA_TNUMBER) {
            // Read specified number of bytes
            auto bytes_to_read = lua_tointeger(Lua, i);
            if (bytes_to_read > 0) {
               std::string buffer(bytes_to_read, '\0');
               int bytes_read;
               if (acRead(file, buffer.data(), bytes_to_read, &bytes_read) IS ERR::Okay and bytes_read > 0) {
                  buffer.resize(bytes_read);
                  lua_pushlstring(Lua, buffer.data(), bytes_read);
               }
               else lua_pushnil(Lua);
            }
            else lua_pushstring(Lua, "");
         }
         else luaL_error(Lua, ERR::InvalidType);
      }

      return nargs - 1; // Return number of results (excluding file handle)
   }

   return 0;
}

//********************************************************************************************************************

static int file_write(lua_State *Lua)
{
   if (auto handle = check_file_handle(Lua, 1)) {
      auto file = (objFile *)GetObjectPtr(handle->file_id);
      if (not file) luaL_error(Lua, ERR::InvalidState);

      int nargs = lua_gettop(Lua);
      for (int i = 2; i <= nargs; i++) {
         size_t len;
         auto str = luaL_checklstring(Lua, i, &len);

         int result;
         if (acWrite(file, str, len, &result) != ERR::Okay) luaL_error(Lua, ERR::Write);
      }

      lua_pushvalue(Lua, 1); // Return file handle
      return 1;
   }
   else luaL_error(Lua, ERR::File);
}

//********************************************************************************************************************

static int file_close(lua_State *Lua)
{
   return io_close(Lua);
}

//********************************************************************************************************************

static int file_flush(lua_State *Lua)
{
   if (auto handle = check_file_handle(Lua, 1)) {
      acFlush(GetObjectPtr(handle->file_id));
      lua_pushboolean(Lua, 1);
      return 1;
   }
   else luaL_error(Lua, ERR::File);
}

//********************************************************************************************************************

static int file_seek(lua_State *Lua)
{
   if (auto handle = check_file_handle(Lua, 1)) {
      auto file = (objFile *)GetObjectPtr(handle->file_id);
      if (not file) luaL_error(Lua, ERR::InvalidState);

      auto whence_str = luaL_optstring(Lua, 2, "cur");
      auto offset = luaL_optnumber(Lua, 3, 0);

      auto whence = SEEK::CURRENT;
      if (iequals("set", whence_str)) whence = SEEK::START;
      else if (iequals("cur", whence_str)) whence = SEEK::CURRENT;
      else if (iequals("end", whence_str)) whence = SEEK::END;

      if (acSeek(file, offset, whence) IS ERR::Okay) {
         lua_pushnumber(Lua, file->Position);
         return 1;
      }
      else luaL_error(Lua, ERR::Seek);
   }
   else luaL_error(Lua, ERR::File);
}

//********************************************************************************************************************

static int file_lines(lua_State *Lua)
{
   if (auto handle = check_file_handle(Lua, 1)) {
      if (not handle->file_id) luaL_error(Lua, ERR::InvalidState);

      // Create iterator state - don't close the file when iteration ends since it's a file method
      auto iter = (LinesIterator *)lua_newuserdata(Lua, sizeof(LinesIterator));
      new(iter) LinesIterator(handle, false);

      // Set up GC metamethod for iterator state
      lua_newtable(Lua);
      lua_pushcfunction(Lua, lines_iterator_gc);
      lua_setfield(Lua, -2, "__gc");
      lua_setmetatable(Lua, -2);

      // Return the iterator function with the state as upvalue
      lua_pushcclosure(Lua, lines_iterator, 1);
      return 1;
   }
   else luaL_error(Lua, ERR::File);
}

//********************************************************************************************************************
// Usage: content = io.readAll(path)
// Reads an entire file and returns its content as a string.  Raises an error if the file cannot be opened or read.

static int io_readAll(lua_State *Lua)
{
   auto path = luaL_checkstring(Lua, 1);

   auto file = objFile::create::local({ fl::Path(path), fl::Flags(FL::READ) });
   if (not file) luaL_error(Lua, ERR::OpenFile, "Failed to open file: %s", path);

   file->seekEnd(0);
   auto file_size = file->Position;
   file->seekStart(0);

   if (file_size > 0) {
      std::string buffer(file_size, '\0');
      int bytes_read;
      if (acRead(file, buffer.data(), file_size, &bytes_read) IS ERR::Okay and bytes_read IS file_size) {
         lua_pushlstring(Lua, buffer.data(), bytes_read);
         return 1;
      }
      luaL_error(Lua, ERR::Read, "Failed to read %" PRId64 " bytes from \"%s\"", file_size, path);
   }

   lua_pushstring(Lua, "");
   return 1;
}

//********************************************************************************************************************
// Usage: io.writeAll(path, content)
// Writes content to a file, creating or overwriting it.

static int io_writeAll(lua_State *Lua)
{
   auto path = luaL_checkstring(Lua, 1);
   size_t len;
   auto content = luaL_checklstring(Lua, 2, &len);

   auto file = objFile::create::local({ fl::Path(path), fl::Flags(FL::WRITE|FL::NEW) });
   if (not file) luaL_error(Lua, ERR::CreateFile, "Failed to create file: %s", path);

   int result;
   if (acWrite(file, content, len, &result) != ERR::Okay) {
      luaL_error(Lua, ERR::Write, "Failed to write to file: %s", path);
   }

   return 0;
}

//********************************************************************************************************************
// Usage: bool = io.isFolder(path)
// Returns true if the path string ends with a folder separator ('/', '\\' or ':').

static int io_isFolder(lua_State *Lua)
{
   auto path = luaL_checkstring(Lua, 1);
   auto len = strlen(path);

   if (len > 0) {
      auto last = path[len - 1];
      lua_pushboolean(Lua, (last IS '/') or (last IS '\\') or (last IS ':'));
   }
   else lua_pushboolean(Lua, 0);

   return 1;
}

//********************************************************************************************************************
// Usage: dir, file = io.splitPath(path)
// Splits a path into directory and filename components.  The directory includes the trailing separator.
// Returns nil, nil if path is nil/empty.  Returns nil, path if no separator is found.

static int io_splitPath(lua_State *Lua)
{
   if (lua_gettop(Lua) IS 0 or lua_isnil(Lua, 1)) {
      lua_pushnil(Lua);
      lua_pushnil(Lua);
      return 2;
   }

   auto path = luaL_checkstring(Lua, 1);
   auto len = strlen(path);

   // Find the last path separator
   int sep_pos = -1;
   for (int i = int(len) - 1; i >= 0; i--) {
      if (path[i] IS '/' or path[i] IS '\\' or path[i] IS ':') {
         sep_pos = i;
         break;
      }
   }

   if (sep_pos < 0) {
      lua_pushnil(Lua);
      lua_pushstring(Lua, path);
   }
   else {
      lua_pushlstring(Lua, path, sep_pos + 1); // Directory including separator
      lua_pushstring(Lua, path + sep_pos + 1); // Filename after separator
   }

   return 2;
}

//********************************************************************************************************************
// Usage: cleaned = io.sanitisePath(path)
// Removes repeated consecutive path separator characters ('/', '\\', ':') from a path.

static int io_sanitisePath(lua_State *Lua)
{
   auto path = luaL_checkstring(Lua, 1);
   auto len = strlen(path);

   std::string result;
   result.reserve(len);

   bool prev_was_sep = false;
   for (size_t i = 0; i < len; i++) {
      bool is_sep = (path[i] IS '/' or path[i] IS '\\' or path[i] IS ':');
      if (is_sep and prev_was_sep) continue;
      result += path[i];
      prev_was_sep = is_sep;
   }

   lua_pushlstring(Lua, result.data(), result.size());
   return 1;
}

//********************************************************************************************************************

void register_io_class(lua_State *Lua)
{
   static const struct luaL_Reg iolib_functions[] = {
      { "close",        io_close },
      { "flush",        io_flush },
      { "input",        io_input },
      { "isFolder",     io_isFolder },
      { "lines",        io_lines },
      { "open",         io_open },
      { "output",       io_output },
      { "openPipe",     io_openPipe }, //
      { "read",         io_read },
      { "readAll",      io_readAll },
      { "sanitisePath", io_sanitisePath },
      { "splitPath",    io_splitPath },
      { "tempFile",     io_tempFile },
      { "type",         io_type },
      { "write",        io_write },
      { "writeAll",     io_writeAll },
      { nullptr, nullptr }
   };

   static const struct luaL_Reg file_methods[] = {
      { "read",        file_read },
      { "write",       file_write },
      { "close",       file_close },
      { "flush",       file_flush },
      { "seek",        file_seek },
      { "lines",       file_lines },
      { "__gc",        file_gc },
      { nullptr, nullptr }
   };

   pf::Log log(__FUNCTION__);
   log.trace("Registering io interface.");

   // Create file handle metatable
   luaL_newmetatable(Lua, "Tiri.file");
   lua_pushstring(Lua, "__index");
   lua_pushvalue(Lua, -2);  // pushes the metatable
   lua_settable(Lua, -3);   // metatable.__index = metatable
   luaL_openlib(Lua, nullptr, file_methods, 0);

   // Create io metatable
   luaL_newmetatable(Lua, "Tiri.io");
   lua_pushstring(Lua, "__index");
   lua_pushvalue(Lua, -2);  // pushes the metatable created earlier
   lua_settable(Lua, -3);   // metatable.__index = metatable

   luaL_openlib(Lua, "io", iolib_functions, 0);

   // Add stdin, stdout, stderr constants
   lua_pushnumber(Lua, CONST_STDIN);
   lua_setfield(Lua, -2, "stdin");

   lua_pushnumber(Lua, CONST_STDOUT);
   lua_setfield(Lua, -2, "stdout");

   lua_pushnumber(Lua, CONST_STDERR);
   lua_setfield(Lua, -2, "stderr");

   // Register io interface prototypes for compile-time type inference
   reg_iface_prototype("io", "open", { TiriType::Any }, { TiriType::Str, TiriType::Str });
   reg_iface_prototype("io", "close", { TiriType::Bool }, { TiriType::Any });
   reg_iface_prototype("io", "read", { TiriType::Any }, { TiriType::Any });
   reg_iface_prototype("io", "write", {}, { TiriType::Any });
   reg_iface_prototype("io", "flush", { TiriType::Bool }, {});
   reg_iface_prototype("io", "input", { TiriType::Any }, { TiriType::Any });
   reg_iface_prototype("io", "output", { TiriType::Any }, { TiriType::Any });
   reg_iface_prototype("io", "lines", { TiriType::Func }, { TiriType::Any });
   reg_iface_prototype("io", "openPipe", {}, { TiriType::Str });
   reg_iface_prototype("io", "tempFile", { TiriType::Any }, {});
   reg_iface_prototype("io", "type", { TiriType::Str }, { TiriType::Any });
   reg_iface_prototype("io", "readAll", { TiriType::Str }, { TiriType::Str });
   reg_iface_prototype("io", "writeAll", {}, { TiriType::Str, TiriType::Str });
   reg_iface_prototype("io", "isFolder", { TiriType::Bool }, { TiriType::Str });
   reg_iface_prototype("io", "splitPath", { TiriType::Str, TiriType::Str }, { TiriType::Str });
   reg_iface_prototype("io", "sanitisePath", { TiriType::Str }, { TiriType::Str });
}
