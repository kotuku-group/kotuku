/*********************************************************************************************************************

This source code is placed in the public domain under no warranty from its authors.

**********************************************************************************************************************

-MODULE-
Tiri: Tiri is a customised scripting language for the Script class.

Tiri is a custom scripting language for Kotuku developers.  It is implemented on the backbone of LuaJIT, a
high performance version of the Lua scripting language.  It supports garbage collection, dynamic typing and a 64-bit
byte-code interpreter for compiled code.

Tiri files use the `.tiri` file extension.  Ideally, scripts should start with the comment '-- $TIRI' near
the start of the document so that it can be correctly identified by the Tiri class.

For more information on the Tiri syntax, please refer to the official Tiri Reference Manual.

-END-

*********************************************************************************************************************/

#ifndef NDEBUG
#undef DEBUG
#endif

#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/display.h>
#include <kotuku/modules/tiri.h>
#include <kotuku/modules/regex.h>
#include <kotuku/strings.hpp>

#include <inttypes.h>
#include <vector>
#include <iterator>
#include <mutex>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "parser/parser.h"
#include "lj_bc.h"
#include "lj_array.h"
#include "lj_gc.h"
#include "lj_object.h"

#include "hashes.h"

JUMPTABLE_CORE
JUMPTABLE_REGEX

#include "defs.h"

OBJECTPTR modDisplay = nullptr; // Required by tiri_input.c
OBJECTPTR modTiri = nullptr;
OBJECTPTR modRegex = nullptr;
OBJECTPTR clTiri = nullptr;
OBJECTPTR glTiriContext = nullptr;
struct ActionTable *glActions = nullptr;
bool glPrintMsg = false;
JOF glJitOptions = JOF::NIL;
ankerl::unordered_dense::map<std::string_view, ACTIONID, CaseInsensitiveHashView, CaseInsensitiveEqualView> glActionLookup;
ankerl::unordered_dense::map<std::string_view, uint32_t> glStructSizes;
ankerl::unordered_dense::map<uint32_t, TiriConstant> glConstantRegistry;
ankerl::unordered_dense::map<struct_name, struct_record, struct_hash> glStructs;
std::shared_mutex glConstantMutex;

static struct MsgHandler *glMsgThread = nullptr; // Message handler for thread callbacks

constexpr auto HASH_TRACE_TOKENS         = pf::strhash("trace-tokens");
constexpr auto HASH_TRACE_EXPECT         = pf::strhash("trace-expect");
constexpr auto HASH_TRACE_BOUNDARY       = pf::strhash("trace-boundary");
constexpr auto HASH_TRACE_OPERATORS      = pf::strhash("trace-operators");
constexpr auto HASH_TRACE_REGISTERS      = pf::strhash("trace-registers");
constexpr auto HASH_TRACE_CFG            = pf::strhash("trace-cfg");
constexpr auto HASH_TRACE_ASSIGNMENTS    = pf::strhash("trace-assignments");
constexpr auto HASH_TRACE_VALUE_CATEGORY = pf::strhash("trace-value-category");
constexpr auto HASH_TRACE_TYPES          = pf::strhash("trace-types");
constexpr auto HASH_DIAGNOSE             = pf::strhash("diagnose");
constexpr auto HASH_DUMP_BYTECODE        = pf::strhash("dump-bytecode");
constexpr auto HASH_PROFILE              = pf::strhash("profile");
constexpr auto HASH_TRACE                = pf::strhash("trace");
constexpr auto HASH_TOP_TIPS             = pf::strhash("top-tips");
constexpr auto HASH_TIPS                 = pf::strhash("tips");
constexpr auto HASH_ALL_TIPS             = pf::strhash("all-tips");
constexpr auto HASH_OFF                  = pf::strhash("off");

#include "module_def.cpp"

//********************************************************************************************************************

APTR get_meta(lua_State *Lua, int Arg, CSTRING MetaTable)
{
   if (auto address = (struct object *)lua_touserdata(Lua, Arg)) {
      if (lua_getmetatable(Lua, Arg)) {  // does it have a metatable?
         lua_getfield(Lua, LUA_REGISTRYINDEX, MetaTable);  // get correct metatable
         if (lua_rawequal(Lua, -1, -2)) {  // does it have the correct mt?
            lua_pop(Lua, 2);
            return address;
         }
         lua_pop(Lua, 2);
      }
   }

   return nullptr;
}

//********************************************************************************************************************
// Returns a pointer to an object (if the object exists).  To guarantee safety, object access always utilises the ID
// so that we don't run into issues if the object has been collected.

OBJECTPTR access_object(GCobject *Object)
{
   if (Object->accesscount) {
      Object->accesscount++;
      return Object->ptr;
   }
   else if (not Object->uid) return nullptr; // Object reference is dead
   else if ((not Object->ptr) or Object->is_detached()) {
      // Detached objects are always accessed via UID, even if we have a pointer reference.
      OBJECTPTR obj_ptr;
      if (auto error = AccessObject(Object->uid, 5000, &obj_ptr); error IS ERR::Okay) {
         Object->ptr = obj_ptr;
         Object->set_locked(true);
      }
      else if (error IS ERR::DoesNotExist) {
         pf::Log log(__FUNCTION__);
         log.trace("Object #%d has been terminated.", Object->uid);
         Object->ptr = nullptr;
         Object->uid = 0;
      }
   }
   else if (auto error = Object->ptr->lock(); error != ERR::Okay) {
      pf::Log("access_object").warning("#%d lock() failed: %s, Queue: %d", Object->uid, GetErrorMsg(error), Object->ptr->Queue.load());
      return nullptr;
   }

   if (Object->ptr) {
      #if LUA_USE_ASSERT
         // This error indicates that the object was forcibly terminated (e.g. because the parent was terminated).
         // The client should have marked the object as detached in order to prevent this issue.
         lj_assertX(Object->ptr->Class, "Object terminated while still attached.");
      #endif
      Object->accesscount++;
   }

   return Object->ptr;
}

void release_object(GCobject *Object)
{
   if (Object->accesscount > 0) {
      if (--Object->accesscount IS 0) {
         if (Object->is_locked()) {
            ReleaseObject(Object->ptr);
            Object->set_locked(false);
            Object->ptr = nullptr;
         }
         else {
            #ifndef NDEBUG
            if (Object->ptr->Queue.load() <= 0) {
               pf::Log("release_object").warning("#%d Queue underflow before unlock: Queue: %d, ThreadID: %d, OurThread: %d",
                  Object->uid, Object->ptr->Queue.load(), Object->ptr->ThreadID.load(), pf::_get_thread_id());
               DEBUG_BREAK
            }
            #endif
            Object->ptr->unlock();
         }
      }
   }
}

//********************************************************************************************************************
// Automatically load the definitions for the given metaclass, if it has not been loaded already.

void load_include_for_class(lua_State *Lua, objMetaClass *MetaClass)
{
   // Ensure that the base-class is loaded first, if applicable
   if (MetaClass->BaseClassID != MetaClass->ClassID) {
      if (auto base_class = FindClass(MetaClass->BaseClassID)) {
         load_include_for_class(Lua, base_class);
      }
   }

   CSTRING module_name;
   if (auto error = MetaClass->get(FID_Module, module_name); error IS ERR::Okay) {
      if (auto error = load_include(Lua->script, module_name); error != ERR::Okay) {
         luaL_error(Lua, error, "Failed to process module '%s' for class '%s'", module_name, MetaClass->ClassName);
      }
   }
   else pf::Log(__FUNCTION__).traceWarning("Failed to get module name from class '%s', \"%s\"", MetaClass->ClassName, GetErrorMsg(error));
}

//********************************************************************************************************************

[[nodiscard]] static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   pf::Log log;

   CoreBase = argCoreBase;

   glTiriContext = CurrentContext();
   glPrintMsg = GetResource(RES::LOG_LEVEL) >= 4;

   argModule->get(FID_Root, modTiri);

   ActionList(&glActions, nullptr); // Get the global action table from the Core

   // Create a lookup table for converting named actions to IDs.

   for (int action_id=1; glActions[action_id].Name; action_id++) {
      glActionLookup[glActions[action_id].Name] = AC(action_id);
   }

   pf::vector<std::string> *pargs;
   auto task = CurrentTask();
   if ((task->get(FID_Parameters, pargs) IS ERR::Okay) and (pargs)) {
      pf::vector<std::string> &args = *pargs;
      for (int i=0; i < std::ssize(args); i++) {
         if (pf::startswith(args[i], "--jit-options")) {
            // Parse --jit-options [csv] parameter
            // Use in conjunction with --log-api to see the log messages.
            // These options are system-wide, alternatively you can set JitOptions in the Script object.
            std::string value;

            if (i + 1 < std::ssize(args)) {
               value = args[i + 1];
               i++;
            }

            if (not value.empty()) {
               // Split the CSV string and set appropriate global variables
               std::vector<std::string> options;
               pf::split(value, std::back_inserter(options), ',');

               glJitOptions = JOF::NIL;
               for (const auto &option : options) {
                  std::string trimmed = option;
                  pf::trim(trimmed);

                  auto hash = pf::strhash(trimmed);
                  if (hash IS HASH_TRACE_VALUE_CATEGORY)   glJitOptions |= JOF::TRACE_VALUE_CATEGORY;
                  else if (hash IS HASH_TRACE_ASSIGNMENTS) glJitOptions |= JOF::TRACE_ASSIGNMENTS;
                  else if (hash IS HASH_TRACE_OPERATORS) glJitOptions |= JOF::TRACE_OPERATORS;
                  else if (hash IS HASH_TRACE_REGISTERS) glJitOptions |= JOF::TRACE_REGISTERS;
                  else if (hash IS HASH_TRACE_BOUNDARY) glJitOptions |= JOF::TRACE_BOUNDARY;
                  else if (hash IS HASH_TRACE_TOKENS)  glJitOptions |= JOF::TRACE_TOKENS;
                  else if (hash IS HASH_TRACE_EXPECT)  glJitOptions |= JOF::TRACE_EXPECT;
                  else if (hash IS HASH_TRACE_CFG)     glJitOptions |= JOF::TRACE_CFG;
                  else if (hash IS HASH_TRACE_TYPES)   glJitOptions |= JOF::TRACE_TYPES;
                  else if (hash IS HASH_DIAGNOSE)      glJitOptions |= JOF::DIAGNOSE;
                  else if (hash IS HASH_DUMP_BYTECODE) glJitOptions |= JOF::DUMP_BYTECODE;
                  else if (hash IS HASH_PROFILE)       glJitOptions |= JOF::PROFILE;
                  else if (hash IS HASH_TRACE)         glJitOptions |= JOF::TRACE;
                  else if (hash IS HASH_TOP_TIPS)      glJitOptions |= JOF::TOP_TIPS;
                  else if (hash IS HASH_TIPS)          glJitOptions |= JOF::TIPS;
                  else if (hash IS HASH_ALL_TIPS)      glJitOptions |= JOF::ALL_TIPS;
                  else if (hash IS HASH_OFF)           glJitOptions |= JOF::DISABLE_JIT;
                  else log.warning("Unknown JIT option \"%s\" specified.", trimmed.c_str());
               }

               log.msg("JIT options \"%s\" set to $%.8x", value.c_str(), (uint32_t)glJitOptions);

               if ((glJitOptions & (JOF::TRACE|JOF::PROFILE)) != JOF::NIL) {
                  if (GetResource(RES::LOG_LEVEL) < 5) {
                     // Automatically raise the log level to see JIT messages.  Helpful for AI
                     // agents that forget this requirement.
                     SetResource(RES::LOG_LEVEL, 5);
                  }
               }
            }
            else log.warning("No value for --jit-options");
         }
      }
   }

   return create_tiri();
}

static ERR MODExpunge(void)
{
   if (glMsgThread) { FreeResource(glMsgThread); glMsgThread = nullptr; }
   if (clTiri)      { FreeResource(clTiri); clTiri = nullptr; }
   if (modDisplay)  { FreeResource(modDisplay); modDisplay = nullptr; }
   if (modRegex)    { FreeResource(modRegex); modRegex = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR MODOpen(OBJECTPTR Module)
{
   Module->set(FID_FunctionList, glFunctions);
   return ERR::Okay;
}

//********************************************************************************************************************

#ifdef ENABLE_UNIT_TESTS
extern void indexing_unit_tests(int &, int &);
extern void vm_asm_unit_tests(int &, int &);
extern void jit_frame_unit_tests(int &, int &);
extern void parser_unit_tests(int &, int &);
extern void array_unit_tests(int &, int &);
#endif

static void MODTest(CSTRING Options, int *Passed, int *Total)
{
#ifdef ENABLE_UNIT_TESTS
   {
      pf::Log log("TiriTests");
      log.branch("Running indexing unit tests...");
      indexing_unit_tests(*Passed, *Total);
   }
   {
      pf::Log log("TiriTests");
      log.branch("Running parser unit tests...");
      parser_unit_tests(*Passed, *Total);
   }
   {
      pf::Log log("TiriTests");
      log.branch("Running VM assembly unit tests...");
      vm_asm_unit_tests(*Passed, *Total);
   }
   {
      pf::Log log("TiriTests");
      log.branch("Running JIT frame unit tests...");
      jit_frame_unit_tests(*Passed, *Total);
   }
   {
      pf::Log log("TiriTests");
      log.branch("Running array unit tests...");
      array_unit_tests(*Passed, *Total);
   }
#else
   pf::Log("TiriTests").warning("Unit tests are disabled in this build.");
#endif
}

//********************************************************************************************************************
// Bytecode names for debugging purposes

CSTRING const glBytecodeNames[] = {
#define BCNAME(name, ma, mb, mc, mt) #name,
   BCDEF(BCNAME)
#undef BCNAME
};

/*********************************************************************************************************************

-FUNCTION-
SetVariable: Sets any variable in a loaded Tiri script.

The SetVariable() function provides a method for setting global variables in a Tiri script prior to execution of that
script.  If the script is cached, the variable settings will be available on the next activation.

-INPUT-
obj(Script) Script: Pointer to a Tiri script.
cstr Name: The name of the variable to set.
int Type: A valid field type must be indicated, e.g. `FD_STRING`, `FD_POINTER`, `FD_INT`, `FD_DOUBLE`, `FD_INT64`.
tags Variable: A variable that matches the indicated `Type`.

-ERRORS-
Okay: The variable was defined successfully.
Args:
FieldTypeMismatch: A valid field type was not specified in the `Type` parameter.
ObjectCorrupt: Privately maintained memory has become inaccessible.
-END-

*********************************************************************************************************************/
namespace fl {
ERR SetVariable(objScript *Script, CSTRING Name, int Type, ...)
{
   pf::Log log(__FUNCTION__);
   prvTiri *prv;
   va_list list;

   if ((not Script) or (Script->classID() != CLASSID::TIRI) or (not Name) or (not *Name)) return log.warning(ERR::Args);

   log.branch("Script: %d, Name: %s, Type: $%.8x", Script->UID, Name, Type);

   if (not (prv = (prvTiri *)Script->ChildPrivate)) return log.warning(ERR::ObjectCorrupt);
   if (not prv->Lua) return log.warning(ERR::InvalidState);

   va_start(list, Type);

   if (Type & FD_STRING)       lua_pushstring(prv->Lua, va_arg(list, STRING));
   else if (Type & FD_POINTER) lua_pushlightuserdata(prv->Lua, va_arg(list, APTR));
   else if (Type & FD_INT)     lua_pushinteger(prv->Lua, va_arg(list, int));
   else if (Type & FD_INT64)   lua_pushnumber(prv->Lua, va_arg(list, int64_t));
   else if (Type & FD_DOUBLE)  lua_pushnumber(prv->Lua, va_arg(list, double));
   else {
      va_end(list);
      return log.warning(ERR::FieldTypeMismatch);
   }

   lua_setglobal(prv->Lua, Name);

   va_end(list);
   return ERR::Okay;
}
}
//********************************************************************************************************************

void hook_debug(lua_State *Lua, lua_Debug *Info)
{
   pf::Log log("Lua");

   if (Info->event IS LUA_HOOKCALL) {
      if (lua_getinfo(Lua, "nSl", Info)) {
         if (Info->name) log.msg("%s: %s.%s(), Line: %d", Info->what, Info->namewhat, Info->name, Lua->script->CurrentLine + Lua->script->LineOffset);
      }
      else log.warning("lua_getinfo() failed.");
   }
   else if (Info->event IS LUA_HOOKRET) { }
   else if (Info->event IS LUA_HOOKTAILRET) { }
   else if (Info->event IS LUA_HOOKLINE) {
      Lua->script->CurrentLine = Info->currentline - 1; // Our line numbers start from zero
      if (Lua->script->CurrentLine < 0) Lua->script->CurrentLine = 0; // Just to be certain :-)
/*
      if (lua_getinfo(Lua, "nSl", Info)) {
         log.msg("Line %d: %s: %s", Info->currentline, Info->what, Info->name);
      }
      else log.warning("lua_getinfo() failed.");
*/
   }
}

//********************************************************************************************************************
// Builds an array from a fixed list of values.  Guaranteed to always return an array, empty or not.
// Intended for primitives only, for structs please use make_struct_[ptr|serial]_table() because the struct name
// will be required.

void make_array(lua_State *Lua, AET Type, int Elements, CPTR Data, std::string_view StructName)
{
   pf::Log log(__FUNCTION__);

   log.traceBranch("Type: $%.8x, Elements: %d, Data: %p", int(Type), Elements, Data);

   if (Elements < 0) {
      if (not Data) Elements = 0;
      else {
         int i = 0;
         switch (Type) {
            case AET::CSTR:
            case AET::PTR:
            case AET::OBJECT:
               for (i=0; ((APTR *)Data)[i]; i++);
               break;
            case AET::FLOAT:
            case AET::INT32:
               for (i=0; ((int *)Data)[i]; i++);
               break;
            case AET::DOUBLE:
            case AET::INT64:
               for (i=0; ((int64_t *)Data)[i]; i++);
               break;
            case AET::INT16:
               for (i=0; ((int16_t *)Data)[i]; i++);
               break;
            case AET::BYTE:
               for (i=0; ((int8_t *)Data)[i]; i++);
               break;
            case AET::STRUCT: // Use make_struct_*() interfaces instead
            case AET::STR_GC:
            case AET::STR_CPP:
            default:
               log.warning("Unsupported type $%.8x", int(Type));
               lua_pushnil(Lua);
               return;
         }

         Elements = i;
      }
   }

   // lj_array_new() with ARRAY_CACHED handles all data copying internally, including string caching

   GCarray *array = lj_array_new(Lua, Elements, Type, (void *)Data, ARRAY_CACHED, StructName);

   // Push to the stack
   lj_gc_check(Lua);
   setarrayV(Lua, Lua->top++, array);
}

//********************************************************************************************************************
// Create a Lua array from a list of structure pointers.

void make_struct_ptr_array(lua_State *Lua, std::string_view StructName, int Elements, CPTR *Values)
{
   pf::Log log(__FUNCTION__);

   log.trace("%.*s, Elements: %d, Values: %p", int(StructName.size()), StructName.data(), Elements, Values);

   if (Elements < 0) {
      int i;
      for (i=0; Values[i]; i++);
      Elements = i;
   }

   auto s_name = struct_name(StructName);
   if (not glStructs.contains(s_name)) luaL_error(Lua, ERR::Search, "Failed to find struct '%.*s'", int(StructName.size()), StructName.data());

   GCarray *arr = lj_array_new(Lua, Elements, AET::TABLE);
   setarrayV(Lua, Lua->top++, arr); // Push to stack immediately to protect from GC during loop
   int arr_idx = lua_gettop(Lua);

   if (Values) {
      std::vector<lua_ref> ref;
      auto &sdef = glStructs[s_name];

      for (int i=0; i < Elements; i++) {
         if (struct_to_table(Lua, ref, sdef, Values[i]) IS ERR::Okay) {
            // Table is now on top of stack; retrieve arr from stack in case GC moved it
            arr = arrayV(Lua->base + arr_idx - 1);
            TValue *tv = Lua->top - 1;
            GCtab *tab = tabV(tv);
            setgcref(arr->get<GCRef>()[i], obj2gco(tab));
            lj_gc_objbarrier(Lua, arr, tab);
            Lua->top--;  // Pop the table
         }
         else {
            arr = arrayV(Lua->base + arr_idx - 1);
            setgcrefnull(arr->get<GCRef>()[i]);
         }
      }
   }
}

//********************************************************************************************************************
// Create an array from a serialised list of structures aligned to a 64-bit boundary.

void make_struct_serial_array(lua_State *Lua, std::string_view StructName, int Elements, CPTR Input)
{
   pf::Log log(__FUNCTION__);

   if (Elements < 0) Elements = 0; // The total number of structs is a hard requirement.

   auto s_name = struct_name(StructName);
   if (not glStructs.contains(s_name)) luaL_error(Lua, ERR::Search, "Failed to find struct '%.*s'", int(StructName.size()), StructName.data());

   GCarray *arr = lj_array_new(Lua, Elements, AET::TABLE);
   setarrayV(Lua, Lua->top++, arr); // Push to stack immediately to protect from GC during loop
   int arr_idx = lua_gettop(Lua);

   if (Input) {
      std::vector<lua_ref> ref;
      auto &sdef = glStructs[s_name];

      // 64-bit compilers don't always align structures to 64-bit, and it's difficult to compute alignment with
      // certainty.  It is essential that structures that are intended to be serialised into arrays are manually
      // padded to 64-bit so that the potential for mishap is eliminated.

      int def_size = ALIGN64(sdef.Size);
      char aligned = ((sdef.Size & 0x7) != 0) ? 'N': 'Y';
      if (aligned IS 'N') {
         log.msg("%.*s, Elements: %d, Values: %p, StructSize: %d, Aligned: %c", int(StructName.size()), StructName.data(), Elements, Input, def_size, aligned);
      }

      for (int i=0; i < Elements; i++) {
         if (struct_to_table(Lua, ref, sdef, Input) IS ERR::Okay) {
            // Table is now on top of stack; retrieve arr from stack in case GC moved it
            arr = arrayV(Lua->base + arr_idx - 1);
            TValue *tv = Lua->top - 1;
            GCtab *tab = tabV(tv);
            setgcref(arr->get<GCRef>()[i], obj2gco(tab));
            lj_gc_objbarrier(Lua, arr, tab);
            Lua->top--;  // Pop the table
         }
         else {
            arr = arrayV(Lua->base + arr_idx - 1);
            setgcrefnull(arr->get<GCRef>()[i]);
         }

         Input = (int8_t *)Input + def_size;
      }
   }
}

//********************************************************************************************************************
// The TypeName can be in the format 'Struct:Arg' without causing any issues.

void make_any_array(lua_State *Lua, int Flags, std::string_view TypeName, int Elements, CPTR Values)
{
   if (Flags & FD_STRUCT) {
      if (Flags & FD_POINTER) make_struct_ptr_array(Lua, TypeName, Elements, (CPTR *)Values);
      else make_struct_serial_array(Lua, TypeName, Elements, Values);
   }
   else make_array(Lua, ff_to_aet(Flags), Elements, Values, TypeName);
}

//********************************************************************************************************************

void get_line(objScript *Self, int Line, STRING Buffer, int Size)
{
   if (CSTRING str = Self->String) {
      int i;
      for (i=0; i < Line; i++) {
         if (not (str = next_line(str))) {
            Buffer[0] = 0;
            return;
         }
      }

      while ((*str IS ' ') or (*str IS '\t')) str++;

      for (i=0; i < Size-1; i++) {
         if ((*str IS '\n') or (*str IS '\r') or (not *str)) break;
         Buffer[i] = *str++;
      }
      Buffer[i] = 0;
   }
   else Buffer[0] = 0;
}

//********************************************************************************************************************
// Bytecode read & write callbacks.  Returning 1 will stop processing.

int code_writer_id(lua_State *Lua, CPTR Data, size_t Size, void *FileID)
{
   pf::Log log("code_writer");

   if (Size <= 0) return 0; // Ignore bad size requests

   pf::ScopedObjectLock file((MAXINT)FileID);
   if (file.granted()) {
      if (acWrite(*file, (APTR)Data, Size) IS ERR::Okay) return 0;
   }
   log.warning("Failed writing %d bytes.", (int)Size);
   return 1;
}

int code_writer(lua_State *Lua, CPTR Data, size_t Size, OBJECTPTR File)
{
   pf::Log log(__FUNCTION__);

   if (Size <= 0) return 0; // Ignore bad size requests

   int result;
   if (acWrite(File, (APTR)Data, Size, &result) IS ERR::Okay) {
      if ((size_t)result != Size) {
         log.warning("Wrote %d bytes instead of %d.", result, (int)Size);
         return 1;
      }
      else return 0;
   }
   else {
      log.warning("Failed writing %d bytes.", (int)Size);
      return 1;
   }
}

//********************************************************************************************************************
// Callback for lua_load() to read data from File objects.

CSTRING code_reader(lua_State *Lua, void *Handle, size_t *Size)
{
   auto handle = (code_reader_handle *)Handle;
   int result;
   if (acRead(handle->File, handle->Buffer, SIZE_READ, &result) IS ERR::Okay) {
      *Size = result;
      return (CSTRING)handle->Buffer;
   }
   else return nullptr;
}

//********************************************************************************************************************

#ifndef NDEBUG

static void stack_dump(lua_State *L) __attribute__ ((unused));

static void stack_dump(lua_State *L)
{
   int i;
   int top = lua_gettop(L);
   for (i=1; i <= top; i++) {  // repeat for each level
      int t = lua_type(L, i);
      switch (t) {
         case LUA_TSTRING:  printf("'%s'", lua_tostring(L, i)); break;
         case LUA_TBOOLEAN: printf(lua_toboolean(L, i) ? "true" : "false"); break;
         case LUA_TNUMBER:  printf("%g", lua_tonumber(L, i)); break;
         default:           printf("%s", lua_typename(L, t)); break;
      }
      printf("  ");  // put a separator
   }
   printf("\n");  // end the listing
}

#endif

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, MODOpen, MODExpunge, MODTest, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_tiri_module() { return &ModHeader; }
