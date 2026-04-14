// Native Kōtuku object library.
// Copyright (C) 2025-2026 Paul Manias.
//
// The core's technical design means that any object that is not *directly* owned by the Lua Script must be treated as
// external to that script.  External objects must be locked appropriately whenever they are used.  Locking
// ensures that threads can interact with the object safely and that the object cannot be prematurely terminated.
//
// Only objects created through the standard obj.new() interface are directly accessible without a lock.  Those referenced
// through obj.find(), push_object(), or children created with some_object.new() are marked as detached.
//
// Note: If changing type conversion for field flags to Lua types, there are dependencies that need to be updated
// elsewhere, such as in field_type_lookup.cpp.  Do a file search for "FD_" to find them.

#define lib_object_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_tab.h"
#include "lj_str.h"
#include "lj_meta.h"
#include "lj_object.h"
#include "lj_proto_registry.h"
#include "lib.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string_view>
#include <ranges>
#include <cassert>
#include <kotuku/main.h>
#include <kotuku/strings.hpp>

#include "../../defs.h"
#include "../../hashes.h"

#define LJLIB_MODULE_object

template<class... Args> void RMSG(Args...) {
   //log.trace(Args)  // Enable if you want to debug results returned from functions, actions etc
}

static constexpr uint32_t OJH_init      = simple_hash("init");
static constexpr uint32_t OJH_free      = simple_hash("free");
static constexpr uint32_t OJH_children  = simple_hash("children");
static constexpr uint32_t OJH_detach    = simple_hash("detach");
static constexpr uint32_t OJH_get       = simple_hash("get");
static constexpr uint32_t OJH_new       = simple_hash("new");
static constexpr uint32_t OJH_state     = simple_hash("_state");
static constexpr uint32_t OJH_getKey    = simple_hash("getKey");
static constexpr uint32_t OJH_set       = simple_hash("set");
static constexpr uint32_t OJH_setKey    = simple_hash("setKey");
static constexpr uint32_t OJH_delayCall = simple_hash("delayCall");
static constexpr uint32_t OJH_exists    = simple_hash("exists");
static constexpr uint32_t OJH_subscribe = simple_hash("subscribe");
static constexpr uint32_t OJH_unsubscribe = simple_hash("unsubscribe");

// Forward declarations
[[nodiscard]] static int object_action_call_args(lua_State *);
[[nodiscard]] static int object_method_call_args(lua_State *);
[[nodiscard]] static int object_action_call(lua_State *);
[[nodiscard]] static int object_method_call(lua_State *);
[[nodiscard]] static int get_results(lua_State *, const FunctionField *, const int8_t *);
[[nodiscard]] static ERR set_object_field(lua_State *, OBJECTPTR, uint32_t, int);

[[nodiscard]] static int object_children(lua_State *);
[[nodiscard]] static int object_detach(lua_State *);
[[nodiscard]] static int object_exists(lua_State *);
[[nodiscard]] static int object_free(lua_State *);
[[nodiscard]] static int object_get(lua_State *);
[[nodiscard]] static int object_getkey(lua_State *);
[[nodiscard]] static int object_init(lua_State *);
[[nodiscard]] static int object_newchild(lua_State *);
[[nodiscard]] static int object_set(lua_State *);
[[nodiscard]] static int object_setkey(lua_State *);
[[nodiscard]] static int object_state(lua_State *);
[[nodiscard]] static int object_subscribe(lua_State *);
[[nodiscard]] static int object_unsubscribe(lua_State *);

[[nodiscard]] static int object_close_handler(lua_State *);
[[nodiscard]] static int object_with_lock(lua_State *);

[[nodiscard]] static int object_get_rgb(lua_State *, const obj_read &, GCobject *);
[[nodiscard]] static int object_get_array(lua_State *, const obj_read &, GCobject *);
[[nodiscard]] static int object_get_struct(lua_State *, const obj_read &, GCobject *);
[[nodiscard]] static int object_get_string(lua_State *, const obj_read &, GCobject *);
[[nodiscard]] static int object_get_object(lua_State *, const obj_read &, GCobject *);
[[nodiscard]] static int object_get_ptr(lua_State *, const obj_read &, GCobject *);
[[nodiscard]] static int object_get_double(lua_State *, const obj_read &, GCobject *);
[[nodiscard]] static int object_get_large(lua_State *, const obj_read &, GCobject *);
[[nodiscard]] static int object_get_ulong(lua_State *, const obj_read &, GCobject *);
[[nodiscard]] static int object_get_long(lua_State *, const obj_read &, GCobject *);

[[nodiscard]] static ERR object_set_array(lua_State *, OBJECTPTR, Field *, int);
[[nodiscard]] static ERR object_set_function(lua_State *, OBJECTPTR, Field *, int);
[[nodiscard]] static ERR object_set_object(lua_State *, OBJECTPTR, Field *, int);
[[nodiscard]] static ERR object_set_ptr(lua_State *, OBJECTPTR, Field *, int);
[[nodiscard]] static ERR object_set_double(lua_State *, OBJECTPTR, Field *, int);
[[nodiscard]] static ERR object_set_lookup(lua_State *, OBJECTPTR, Field *, int);
[[nodiscard]] static ERR object_set_oid(lua_State *, OBJECTPTR, Field *, int);
[[nodiscard]] static ERR object_set_number(lua_State *, OBJECTPTR, Field *, int);

inline void SET_CONTEXT(lua_State *Lua, APTR Function) {
   lua_pushvalue(Lua, 1); // Duplicate the object reference
   lua_pushcclosure(Lua, (lua_CFunction)Function, 1); // C function to call, +1 value for the object reference
}

[[nodiscard]] static int stack_object_children(lua_State *Lua, const obj_read &Handle, GCobject *def) { SET_CONTEXT(Lua, (APTR)object_children); return 1; }
[[nodiscard]] static int stack_object_detach(lua_State *Lua, const obj_read &Handle, GCobject *def) { SET_CONTEXT(Lua, (APTR)object_detach); return 1; }
[[nodiscard]] static int stack_object_exists(lua_State *Lua, const obj_read &Handle, GCobject *def) { SET_CONTEXT(Lua, (APTR)object_exists); return 1; }
[[nodiscard]] static int stack_object_free(lua_State *Lua, const obj_read &Handle, GCobject *def) { SET_CONTEXT(Lua, (APTR)object_free); return 1; }
[[nodiscard]] static int stack_object_get(lua_State *Lua, const obj_read &Handle, GCobject *def) { SET_CONTEXT(Lua, (APTR)object_get); return 1; }
[[nodiscard]] static int stack_object_getKey(lua_State *Lua, const obj_read &Handle, GCobject *def) { SET_CONTEXT(Lua, (APTR)object_getkey); return 1; }
[[nodiscard]] static int stack_object_init(lua_State *Lua, const obj_read &Handle, GCobject *def) { SET_CONTEXT(Lua, (APTR)object_init); return 1; }
[[nodiscard]] static int stack_object_newchild(lua_State *Lua, const obj_read &Handle, GCobject *def) { SET_CONTEXT(Lua, (APTR)object_newchild); return 1; }
[[nodiscard]] static int stack_object_set(lua_State *Lua, const obj_read &Handle, GCobject *def) { SET_CONTEXT(Lua, (APTR)object_set); return 1; }
[[nodiscard]] static int stack_object_setKey(lua_State *Lua, const obj_read &Handle, GCobject *def) { SET_CONTEXT(Lua, (APTR)object_setkey); return 1; }
[[nodiscard]] static int stack_object_state(lua_State *Lua, const obj_read &Handle, GCobject *def) { SET_CONTEXT(Lua, (APTR)object_state); return 1; }
[[nodiscard]] static int stack_object_subscribe(lua_State *Lua, const obj_read &Handle, GCobject *def) { SET_CONTEXT(Lua, (APTR)object_subscribe); return 1; }
[[nodiscard]] static int stack_object_unsubscribe(lua_State *Lua, const obj_read &Handle, GCobject *def) { SET_CONTEXT(Lua, (APTR)object_unsubscribe); return 1; }

//********************************************************************************************************************
// Action jump table implementation

static int action_activate(lua_State *Lua)
{
   auto obj_ref = object_context(Lua);
   ERR error = ERR::Okay;
   bool release = false;

   if (obj_ref->ptr) error = Action(AC::Activate, obj_ref->ptr, nullptr);
   else if (auto obj = access_object(obj_ref)) {
      error = Action(AC::Activate, obj, nullptr);
      release = true;
   }

   lua_pushinteger(Lua, int(error));
   if (release) release_object(obj_ref);
   report_action_error(Lua, obj_ref, "Activate", error);
   return 1;
}

static int action_draw(lua_State *Lua)
{
   auto obj_ref = object_context(Lua);

   ERR error = ERR::Okay;
   int8_t argbuffer[sizeof(struct acDraw)+8];

   if ((error = build_args(Lua, glActions[int(AC::Draw)].Args, glActions[int(AC::Draw)].Size, argbuffer, nullptr)) != ERR::Okay) {
      luaL_error(Lua, ERR::Args, "Argument build failed for Draw().");
      return 0;
   }

   bool release = false;
   if (obj_ref->ptr) error = Action(AC::Draw, obj_ref->ptr, argbuffer);
   else if (auto obj = access_object(obj_ref)) {
      error = Action(AC::Draw, obj, argbuffer);
      release = true;
   }

   lua_pushinteger(Lua, int(error));
   if (release) release_object(obj_ref);
   report_action_error(Lua, obj_ref, "Draw", error);
   return 1;
}

static int obj_jump_empty(lua_State *Lua, const obj_read &Handle, GCobject *def) { return 0; }
static int obj_jump_signal(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Signal)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_activate(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushcclosure(Lua, action_activate, 1); return 1; }
static int obj_jump_clear(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Clear)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_copydata(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::CopyData)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_datafeed(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::DataFeed)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_deactivate(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Deactivate)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_draw(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushcclosure(Lua, action_draw, 1); return 1; }
static int obj_jump_flush(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Flush)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_focus(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Focus)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_savesettings(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::SaveSettings)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_getkey(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::GetKey)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_dragdrop(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::DragDrop)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_hide(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Hide)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_lock(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Lock)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_lostfocus(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::LostFocus)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_move(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Move)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_movetoback(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::MoveToBack)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_movetofront(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::MoveToFront)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_redo(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Redo)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_query(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Query)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_read(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Read)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_rename(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Rename)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_reset(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Reset)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_resize(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Resize)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_saveimage(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::SaveImage)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_savetoobject(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::SaveToObject)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_seek(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Seek)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_setkey(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::SetKey)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_show(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Show)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_undo(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Undo)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_unlock(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Unlock)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_next(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Next)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_prev(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Prev)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_write(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Write)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_setfield(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::SetField)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_clipboard(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Clipboard)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_refresh(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Refresh)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_disable(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Disable)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_enable(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Enable)); lua_pushcclosure(Lua, object_action_call, 2); return 1; }
static int obj_jump_redimension(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::Redimension)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }
static int obj_jump_movetopoint(lua_State *Lua, const obj_read &Handle, GCobject *def) { lua_pushvalue(Lua, 1); lua_pushinteger(Lua, int(AC::MoveToPoint)); lua_pushcclosure(Lua, object_action_call_args, 2); return 1; }

static std::array<obj_read::JUMP *, int(AC::END)> glJumpActions = {
   obj_jump_empty,
   obj_jump_signal,
   obj_jump_activate,
   obj_jump_redimension,
   obj_jump_clear,
   obj_jump_empty, // FreeWarning
   obj_jump_enable,
   obj_jump_copydata,
   obj_jump_datafeed,
   obj_jump_deactivate,
   obj_jump_draw,
   obj_jump_flush,
   obj_jump_focus,
   obj_jump_empty, // Free
   obj_jump_savesettings,
   obj_jump_getkey,
   obj_jump_dragdrop,
   obj_jump_hide,
   obj_jump_empty, // Init
   obj_jump_lock,
   obj_jump_lostfocus,
   obj_jump_move,
   obj_jump_movetoback,
   obj_jump_movetofront,
   obj_jump_empty, // NewChild
   obj_jump_empty, // NewOwner
   obj_jump_empty, // NewObject
   obj_jump_redo,
   obj_jump_query,
   obj_jump_read,
   obj_jump_rename,
   obj_jump_reset,
   obj_jump_resize,
   obj_jump_saveimage,
   obj_jump_savetoobject,
   obj_jump_movetopoint,
   obj_jump_seek,
   obj_jump_setkey,
   obj_jump_show,
   obj_jump_undo,
   obj_jump_unlock,
   obj_jump_next,
   obj_jump_prev,
   obj_jump_write,
   obj_jump_setfield,
   obj_jump_clipboard,
   obj_jump_refresh,
   obj_jump_disable
};

#include "../../tiri_objects_indexes.cpp"
#include "../../tiri_objects_calls.cpp"

//********************************************************************************************************************

[[nodiscard]] static int obj_jump_method(lua_State *Lua, const obj_read &Handle, GCobject *def)
{
   lua_pushvalue(Lua, 1);
   lua_pushlightuserdata(Lua, Handle.Data);
   if ((((MethodEntry *)Handle.Data)->Args) and (((MethodEntry *)Handle.Data)->Size)) {
      lua_pushcclosure(Lua, object_method_call_args, 2);
   }
   else lua_pushcclosure(Lua, object_method_call, 2);
   return 1;
}

//********************************************************************************************************************
// Returns the read-access jump table for a class, creating it if not present.  The jump table speeds up access to fields,
// methods, and actions by hashing all accessible string names up-front and matching them with customised function
// calls.  At run-time, the main penalty is reduced to a binary search on the jump table.

READ_TABLE * get_read_table(objMetaClass *Class)
{
   if (not Class->ReadTable.empty()) return &Class->ReadTable;

   pf::ScopedObjectLock lock(Class);

   READ_TABLE &jmp = Class->ReadTable;

   for (auto code : std::views::iota(1, int(AC::END))) {
      auto hash = simple_hash(glActions[code].Name, simple_hash("ac"));
      jmp.push_back(obj_read(hash, glJumpActions[code]));
   }

   MethodEntry *methods;
   int total_methods;
   if (Class->get(FID_Methods, methods, total_methods) IS ERR::Okay) {
      auto methods_span = std::span(methods, total_methods);
      for (auto &method : methods_span | std::views::drop(1)) {
         if (method.MethodID != AC::NIL) {
            auto hash = simple_hash(method.Name, simple_hash("mt"));
            jmp.push_back(obj_read(hash, obj_jump_method, &method));
         }
      }
   }

   Field *dict;
   int total_dict;
   if (Class->get(FID_Dictionary, dict, total_dict) IS ERR::Okay) {
      auto dict_span = std::span(dict, total_dict);
      for (auto &field : dict_span | std::views::filter([](const auto &f) { return f.Flags & FDF_R; })) {
         auto hash = field.FieldID;

         if (field.Flags & FD_ARRAY) {
            if (field.Flags & FD_RGB) jmp.push_back(obj_read(hash, object_get_rgb, &field));
            else jmp.push_back(obj_read(hash, object_get_array, &field));
         }
         else if (field.Flags & FD_STRUCT) jmp.push_back(obj_read(hash, object_get_struct, &field));
         else if (field.Flags & FD_STRING) jmp.push_back(obj_read(hash, object_get_string, &field));
         else if (field.Flags & FD_POINTER) {
            if (field.Flags & (FD_OBJECT|FD_LOCAL)) {
               jmp.push_back(obj_read(hash, object_get_object, &field));
            }
            else jmp.push_back(obj_read(hash, object_get_ptr, &field));
         }
         else if (field.Flags & FD_DOUBLE) jmp.push_back(obj_read(hash, object_get_double, &field));
         else if (field.Flags & FD_INT64) jmp.push_back(obj_read(hash, object_get_large, &field));
         else if (field.Flags & FD_INT) {
            if (field.Flags & FD_UNSIGNED) jmp.push_back(obj_read(hash, object_get_ulong, &field));
            else jmp.push_back(obj_read(hash, object_get_long, &field));
         }
         else if (field.Flags & FD_FUNCTION); // Unsupported
         else pf::Log().warning("Unable to support field %s.%s for reading", Class->Name, field.Name);
      }
   }

   jmp.emplace_back(OJH_init, stack_object_init);
   jmp.emplace_back(OJH_free, stack_object_free);
   jmp.emplace_back(OJH_children, stack_object_children);
   jmp.emplace_back(OJH_detach, stack_object_detach);
   jmp.emplace_back(OJH_get, stack_object_get);
   jmp.emplace_back(OJH_new, stack_object_newchild);
   jmp.emplace_back(OJH_state, stack_object_state);
   jmp.emplace_back(OJH_getKey, stack_object_getKey);
   jmp.emplace_back(OJH_set, stack_object_set);
   jmp.emplace_back(OJH_setKey, stack_object_setKey);
   jmp.emplace_back(OJH_exists, stack_object_exists);
   jmp.emplace_back(OJH_subscribe, stack_object_subscribe);
   jmp.emplace_back(OJH_unsubscribe, stack_object_unsubscribe);

   std::sort(jmp.begin(), jmp.end(), read_hash);

   return &Class->ReadTable;
}

//********************************************************************************************************************
// Get the write table for a class, creating it if not present.

WRITE_TABLE * get_write_table(objMetaClass *Class)
{
   if (not Class->WriteTable.empty()) return &Class->WriteTable;

   pf::ScopedObjectLock lock(Class);

   WRITE_TABLE &jmp = Class->WriteTable;
   Field *dict;
   int total_dict;
   if (Class->get(FID_Dictionary, dict, total_dict) IS ERR::Okay) {
      auto dict_span = std::span(dict, total_dict);
      for (auto &field : dict_span | std::views::filter([](const auto &f) { return f.Flags & (FD_W|FD_I); })) {
         char ch[2] = { field.Name[0], 0 };
         if ((ch[0] >= 'A') and (ch[0] <= 'Z')) ch[0] = ch[0] - 'A' + 'a';
         auto hash = simple_hash(field.Name+1, simple_hash(ch));

         if (field.Flags & FD_ARRAY) {
            jmp.push_back(obj_write(hash, object_set_array, &field));
         }
         else if (field.Flags & FD_FUNCTION) {
            jmp.push_back(obj_write(hash, object_set_function, &field));
         }
         else if (field.Flags & FD_POINTER) {
            if (field.Flags & (FD_OBJECT|FD_LOCAL)) {
               jmp.push_back(obj_write(hash, object_set_object, &field));
            }
            else jmp.push_back(obj_write(hash, object_set_ptr, &field));
         }
         else if (field.Flags & (FD_DOUBLE|FD_FLOAT)) {
            jmp.push_back(obj_write(hash, object_set_double, &field));
         }
         else if (field.Flags & (FD_FLAGS|FD_LOOKUP)) {
            jmp.push_back(obj_write(hash, object_set_lookup, &field));
         }
         else if (field.Flags & FD_OBJECT) {
            jmp.push_back(obj_write(hash, object_set_oid, &field));
         }
         else if (field.Flags & (FD_INT|FD_INT64)) {
            jmp.push_back(obj_write(hash, object_set_number, &field));
         }
      }
   }

   std::sort(jmp.begin(), jmp.end(), write_hash);

   return &Class->WriteTable;
}

//********************************************************************************************************************
// Usage: object.fieldName = newvalue
//
// Using all caps abbreviations is discouraged when designing class fields, e.g. JITOptions won't work out, but
// JitOptions is fine.
//
// NB: This function is also called directly by the thunk implementation in cases where thunks resolve to objects.

extern int object_newindex(lua_State *Lua)
{
   if (auto def = lj_get_object_fast(Lua, 1)) {
      if (auto hash = luaL_checkstringhash(Lua, 2)) {
         if (auto obj = access_object(def)) {
            auto jt = get_write_table(def->classptr);

            ERR error;
            auto func = std::lower_bound(jt->begin(), jt->end(), obj_write(hash), write_hash);
            if ((func != jt->end()) and (func->Hash IS hash)) {
               error = func->Call(Lua, obj, func->Field, 3);
            }
            else error = ERR::NoSupport;
            release_object(def);

            if (error >= ERR::ExceptionThreshold) {
               luaL_error(Lua, error, "Write failure: %s.%s: %s", def->classptr->ClassName, luaL_checkstring(Lua, 2), GetErrorMsg(error));
            }
         }
      }
   }
   return 0;
}

//********************************************************************************************************************
// This is the default path for reading object fields when optimisation is unavailable.

[[nodiscard]] extern int object_index(lua_State *Lua)
{
   auto def = objectV(Lua->base);

   if (not def->uid) luaL_error(Lua, ERR::DoesNotExist, "Object dereferenced, unable to read field.");

   // Get key as GCstr to access precomputed hash
   TValue *tv_key = Lua->base + 1;
   if (not tvisstr(tv_key)) lj_err_argt(Lua, 2, LUA_TSTRING);
   GCstr *keystr = strV(tv_key);

   auto read_table = get_read_table(def->classptr);
   auto hash_key = obj_read(keystr->hash);
   auto func = std::lower_bound(read_table->begin(), read_table->end(), hash_key, read_hash);
   if ((func != read_table->end()) and (func->Hash IS keystr->hash)) {
      auto result = func->Call(Lua, *func, def); // On error, result is 0 and CaughtError is defined.
      if ((not result) and (Lua->CaughtError > ERR::ExceptionThreshold)) luaL_error(Lua, Lua->CaughtError, "Read failure: %s.%s: %s", def->classptr->ClassName, luaL_checkstring(Lua, 2), GetErrorMsg(Lua->CaughtError));
      return result;
   }

   luaL_error(Lua, ERR::NoFieldAccess, "Field does not exist or is unreadable: %s.%s",
      def->classptr ? def->classptr->ClassName: "?", strdata(keystr));

   return 0; // Not reached
}

//********************************************************************************************************************

[[nodiscard]] static ACTIONID get_action_info(lua_State *Lua, CLASSID ClassID, CSTRING action, const FunctionField **Args)
{
   pf::Log log;

   if ((action[0] IS 'm') and (action[1] IS 't')) {
      action += 2;
   }
   else if (auto it = glActionLookup.find(action); it != glActionLookup.end()) {
      *Args = glActions[int(it->second)].Args;
      return it->second;
   }

   *Args = nullptr;
   if (auto mc = FindClass(ClassID)) {
      MethodEntry *table;
      int total_methods;
      ACTIONID action_id;
      if ((mc->get(FID_Methods, table, total_methods) IS ERR::Okay) and (table)) {
         for (int i=1; i < total_methods; i++) {
            if ((table[i].Name) and (iequals(action, table[i].Name))) {
               action_id = table[i].MethodID;
               *Args = table[i].Args;
               i = 0x7ffffff0;
               return action_id;
            }
         }
      }
      else log.warning("No methods declared for class %s, cannot call %s()", mc->ClassName, action);
   }
   else luaL_error(Lua, ERR::Search);

   return AC::NIL;
}

//********************************************************************************************************************
// obj.new("Display", { field1 = value1, field2 = value2, ...})

LJLIB_CF(object_new)
{
   pf::Log log("obj.new");
   CSTRING class_name;
   CLASSID class_id;

   NF objflags = NF::NIL;
   int type = lua_type(L, 1);
   if (type IS LUA_TNUMBER) {
      class_id = CLASSID(lua_tointeger(L, 1));
      class_name = nullptr;
      log.trace("$%.8x", uint32_t(class_id));
   }
   else if ((class_name = luaL_checkstring(L, 1))) {
      class_id = CLASSID(strihash(class_name));
      log.trace("%s, $%.8x", class_name, uint32_t(class_id));
   }
   else luaL_error(L, ERR::Mismatch, "String or ID expected for class name, got '%s'.", lua_typename(L, type));

   OBJECTPTR obj;
   if (auto error = NewObject(class_id, objflags, &obj); error IS ERR::Okay) {
      if (L->script->TargetID) {
         ScopedObjectLock new_owner(L->script->TargetID);
         if (new_owner.granted()) SetOwner(obj, *new_owner);
         else luaL_error(L, ERR::LockFailed);
      }

      obj->CreatorMeta = L;

      load_include_for_class(L, obj->Class);

      lua_pushobject(L, obj->UID, obj, obj->Class, 0);
      if (lua_istable(L, 2)) {
         ERR field_error    = ERR::Okay;
         CSTRING field_name = nullptr;
         int failed_type    = LUA_TNONE;
         lua_pushnil(L);
         while (lua_next(L, 2) != 0) {
            if ((field_name = luaL_checkstring(L, -2))) {
               //if (field_name[0] IS '_') return acSetKey(obj, field_name+1, lua_tostring(Lua, ValueIndex));

               if (iequals("owner", field_name)) field_error = ERR::UnsupportedOwner;
               else field_error = set_object_field(L, obj, fieldhash(field_name), -1);
            }
            else field_error = ERR::UnsupportedField;

            if (field_error != ERR::Okay) {
               failed_type = lua_type(L, -1);
               lua_pop(L, 2);
               break;
            }
            else lua_pop(L, 1);
         }

         if ((field_error != ERR::Okay) or ((error = InitObject(obj)) != ERR::Okay)) {
            class_name = obj->className();
            FreeResource(obj);

            if (field_error != ERR::Okay) {
               luaL_error(L, field_error, "Failed to set field '%s.%s' with %s, error: %s", class_name, field_name, lua_typename(L, failed_type), GetErrorMsg(field_error));
            }
            else luaL_error(L, error, "Failed to Init() %s: %s", class_name, GetErrorMsg(error));

            return 0;
         }
      }

      return 1;
   }
   else luaL_error(L, ERR::NewObject, "NewObject() failed for class '%s', error: %s", class_name, GetErrorMsg(error));

   return 0;
}

//********************************************************************************************************************
// obj.find("ObjectName" | ObjectID, [ClassName | ClassID])

[[nodiscard]] static int object_find_ptr(lua_State *L, OBJECTPTR obj)
{
   load_include_for_class(L, obj->Class);
   lua_pushobject(L, obj->UID, nullptr, obj->Class, GCOBJ_DETACHED);
   return 1;
}

LJLIB_CF(object_find)
{
   pf::Log log("object.find");
   CSTRING object_name;
   CLASSID class_id;
   OBJECTID object_id;

   int type = lua_type(L, 1);
   if ((type IS LUA_TSTRING) and ((object_name = lua_tostring(L, 1)))) {
      int class_type = lua_type(L, 2);
      if (class_type IS LUA_TNUMBER) class_id = CLASSID(lua_tointeger(L, 2));
      else if (class_type IS LUA_TSTRING) class_id = CLASSID(strihash(lua_tostring(L, 2)));
      else class_id = CLASSID::NIL;

      log.trace("obj.find(%s, $%.8x)", object_name, class_id);

      if ((iequals("self", object_name)) and (class_id IS CLASSID::NIL)) {
         return object_find_ptr(L, L->script);
      }
      else if (iequals("owner", object_name)) {
         if (auto obj = L->script->Owner) return object_find_ptr(L, obj);
         else return 0;
      }

      if (FindObject(object_name, class_id, FOF::SMART_NAMES, &object_id) IS ERR::Okay) {
         return object_find_ptr(L, GetObjectPtr(object_id));
      }
      else log.detail("Unable to find object '%s'", object_name);
   }
   else if ((type IS LUA_TNUMBER) and ((object_id = lua_tointeger(L, 1)))) {
      log.trace("obj.find(#%d)", object_id);

      pf::ScopedObjectLock lock(object_id);
      if (lock.granted()) {
         return object_find_ptr(L, *lock);
      }
   }
   else log.warning("String or ID expected for object name, got '%s'.", lua_typename(L, type));

   return 0;
}

//********************************************************************************************************************
// obj.class(object)

LJLIB_CF(object_class)
{
   auto def = objectV(L->base);
   objMetaClass *cl = def->classptr;
   lua_pushobject(L, cl->UID, (OBJECTPTR)cl, cl, GCOBJ_DETACHED);
   return 1;
}

//********************************************************************************************************************
// Guaranteed to not throw exceptions.

ERR push_object_id(lua_State *Lua, OBJECTID ObjectID)
{
   if (not ObjectID) { lua_pushnil(Lua); return ERR::Okay; }

   if (auto object = GetObjectPtr(ObjectID)) {
      lua_pushobject(Lua, ObjectID, nullptr, object->Class, GCOBJ_DETACHED);
      return ERR::Okay;
   }
   else {
      lua_pushobject(Lua, ObjectID, nullptr, nullptr, GCOBJ_DETACHED);
      return ERR::Okay;
   }
}

//********************************************************************************************************************
// Object instance methods (accessed via metatable, not library functions)
// State is maintained globally, so other object variables reference the same state table.

static int object_state(lua_State *Lua)
{
   auto def = object_context(Lua);
   auto prv = (prvTiri *)Lua->script->ChildPrivate;

   pf::Log log(__FUNCTION__);
   if (auto it = prv->StateMap.find(def->uid); it != prv->StateMap.end()) {
      lua_rawgeti(Lua, LUA_REGISTRYINDEX, it->second);
      return 1;
   }
   else {
      lua_createtable(Lua, 0, 0);
      auto state_ref = luaL_ref(Lua, LUA_REGISTRYINDEX);
      prv->StateMap[def->uid] = state_ref;
      lua_rawgeti(Lua, LUA_REGISTRYINDEX, state_ref);
      return 1;
   }
}

//********************************************************************************************************************
// Create a new object as the child of another object.

static int object_newchild(lua_State *Lua)
{
   pf::Log log("obj.child");

   auto parent = object_context(Lua);

   CSTRING class_name;
   CLASSID class_id;
   NF objflags = NF::NIL;
   int type = lua_type(Lua, 1);
   if (type IS LUA_TNUMBER) {
      class_id = CLASSID(lua_tointeger(Lua, 1));
      class_name = nullptr;
      log.trace("$%.8x", class_id);
   }
   else if ((class_name = luaL_checkstring(Lua, 1))) {
      class_id = CLASSID(strihash(class_name));
      log.trace("%s, $%.8x", class_name, class_id);
   }
   else {
      log.warning("String or ID expected for class name, got '%s'.", lua_typename(Lua, type));
      luaL_error(Lua, ERR::Mismatch);
   }

   OBJECTPTR obj;
   if (auto error = NewObject(class_id, objflags, &obj); error IS ERR::Okay) {
      ScopedObjectLock new_owner(Lua->script->TargetID);
      if (new_owner.granted()) SetOwner(obj, *new_owner);
      else luaL_error(Lua, ERR::LockFailed);

      obj->CreatorMeta = Lua;

      load_include_for_class(Lua, obj->Class);

      lua_pushobject(Lua, obj->UID, nullptr, obj->Class, GCOBJ_DETACHED);

      lua_pushinteger(Lua, parent->uid);

      if (set_object_field(Lua, obj, fieldhash("owner"), lua_gettop(Lua)) != ERR::Okay) {
         FreeResource(obj);
         luaL_error(Lua, ERR::SetField);
         return 0;
      }

      lua_pop(Lua, 1);

      if (lua_istable(Lua, 2)) {
         ERR field_error = ERR::Okay;
         CSTRING field_name = nullptr;
         lua_pushnil(Lua);
         while (lua_next(Lua, 2) != 0) {
            if ((field_name = luaL_checkstring(Lua, -2))) {
               //if (field_name[0] IS '_') return acSetKey(obj, field_name+1, lua_tostring(Lua, ValueIndex));

               if (iequals("owner", field_name)) field_error = ERR::UnsupportedOwner;
               else field_error = set_object_field(Lua, obj, fieldhash(field_name), -1);
            }
            else field_error = ERR::UnsupportedField;

            if (field_error != ERR::Okay) {
               lua_pop(Lua, 2);
               break;
            }
            else lua_pop(Lua, 1);
         }

         if ((field_error != ERR::Okay) or ((error = InitObject(obj)) != ERR::Okay)) {
            FreeResource(obj);

            if (field_error != ERR::Okay) {
               luaL_error(Lua, field_error, "Failed to set field '%s', error: %s", field_name, GetErrorMsg(field_error));
            }
            else {
               log.warning("Failed to Init() object '%s', error: %s", class_name, GetErrorMsg(error));
               luaL_error(Lua, ERR::Init);
            }
            return 0;
         }
      }

      return 1;
   }
   else {
      luaL_error(Lua, ERR::NewObject);
      return 0;
   }
}

//********************************************************************************************************************
// Return an array of child object IDs.  Optionally filter by class name or ID, e.g. obj.children("Display")

static int object_children(lua_State *Lua)
{
   pf::Log log("obj.children");

   log.trace("");

   GCobject *def = object_context(Lua);

   CLASSID class_id;
   CSTRING classfilter;
   if ((classfilter = luaL_optstring(Lua, 1, nullptr)) and (classfilter[0])) {
      class_id = CLASSID(strihash(classfilter));
   }
   else class_id = CLASSID::NIL;

   pf::vector<ChildEntry> list;
   if (ListChildren(def->uid, &list) IS ERR::Okay) {
      int index = 0;
      auto id = std::make_unique<int[]>(list.size());
      for (auto &rec : list) {
         if (class_id != CLASSID::NIL) {
            if (rec.ClassID IS class_id) id[index++] = rec.ObjectID;
         }
         else id[index++] = rec.ObjectID;
      }

      make_array(Lua, AET::INT32, index, id.get());
   }
   else make_array(Lua, AET::INT32);

   return 1;
}

//********************************************************************************************************************
// Detach an object, stopping the possibility of it being collected.

static int object_detach(lua_State *Lua)
{
   auto def = object_context(Lua);

   pf::Log log("obj.detach");
   log.traceBranch("Detached: %d", def->is_detached());

   if (not def->is_detached()) def->set_detached(true);

   return 0;
}

//********************************************************************************************************************
// Returns true if the object still exists.

static int object_exists(lua_State *Lua)
{
   auto def = object_context(Lua);
   if (access_object(def)) {
      release_object(def);
      lua_pushboolean(Lua, true);
      return 1;
   }
   return 0;
}

//********************************************************************************************************************

static int object_subscribe(lua_State *Lua)
{
   auto def = object_context(Lua);

   CSTRING action;
   if (not (action = lua_tostring(Lua, 1))) {
      luaL_argerror(Lua, 1, "Action name expected.");
      return 0;
   }

   if (not lua_isfunction(Lua, 2)) {
      luaL_argerror(Lua, 2, "Function expected.");
      return 0;
   }

   const FunctionField *arglist;
   ACTIONID action_id = get_action_info(Lua, def->classptr->ClassID, action, &arglist);

   if (action_id IS AC::NIL) {
      luaL_argerror(Lua, 1, "Action/Method name is invalid.");
      return 0;
   }

   OBJECTPTR obj;
   if (not (obj = access_object(def))) {
      luaL_error(Lua, ERR::AccessObject);
      return 0;
   }

   pf::Log log("obj.subscribe");
   log.trace("Object: %d, Action: %s (ID %d)", def->uid, action, action_id);

   auto callback = C_FUNCTION(notify_action);
   callback.Context = Lua->script;
   if (auto error = SubscribeAction(obj, action_id, &callback); error IS ERR::Okay) {
      auto prv = (prvTiri *)Lua->script->ChildPrivate;
      auto &acsub = prv->ActionList.emplace_back();

      if (not lua_isnil(Lua, 3)) {
         lua_settop(prv->Lua, 3);
         acsub.Reference = luaL_ref(prv->Lua, LUA_REGISTRYINDEX);
      }
      else acsub.Reference = 0;

      lua_settop(prv->Lua, 2);
      acsub.Function = luaL_ref(prv->Lua, LUA_REGISTRYINDEX);
      acsub.Object   = def;
      acsub.Args     = arglist;
      acsub.ObjectID = def->uid;
      acsub.ActionID = action_id;

      release_object(def);
   }
   else {
      release_object(def);
      luaL_error(Lua, error);
   }
   return 0;
}

//********************************************************************************************************************

static int object_unsubscribe(lua_State *Lua)
{
   pf::Log log("unsubscribe");

   auto def = object_context(Lua);

   CSTRING action;
   if (not (action = lua_tostring(Lua, 1))) {
      luaL_argerror(Lua, 1, "Action name expected.");
      return 0;
   }

   const FunctionField *arglist;
   ACTIONID action_id = get_action_info(Lua, def->classptr->ClassID, action, &arglist);

   if (action_id IS AC::NIL) {
      luaL_argerror(Lua, 1, "Action/Method name is invalid.");
      return 0;
   }

   log.trace("Object: %d, Action: %s", def->uid, action);

   auto prv = (prvTiri *)Lua->script->ChildPrivate;
   std::erase_if(prv->ActionList, [&](auto& item) {
      bool should_remove = (item.ObjectID IS def->uid) and ((action_id IS AC::NIL) or (item.ActionID IS action_id));
      if (should_remove) {
         luaL_unref(Lua, LUA_REGISTRYINDEX, item.Function);
         if (item.Reference) luaL_unref(Lua, LUA_REGISTRYINDEX, item.Reference);
      }
      return should_remove;
   });

   return 0;
}

//********************************************************************************************************************

static int object_free(lua_State *Lua)
{
   auto def = object_context(Lua);

   def->flags |= GCOBJ_DETACHED; // Ensure that all future access doesn't go through the ptr

   if (FreeResource(def->uid) IS ERR::InUse) {
      // The object has been marked for termination, automatically freeing it once it has been unlocked and unpinned.
      // Clearing the definition would have adverse effects on any areas that have pinned the object in a thread or
      // closure.  
      return 0;
   }

   def->uid = 0;
   def->ptr = nullptr;
   def->classptr = nullptr;
   def->accesscount = 0;
   return 0;
}

//********************************************************************************************************************

static int object_init(lua_State *Lua)
{
   auto def = object_context(Lua);

   if (auto obj = access_object(def)) {
      auto error = InitObject(obj);
      report_action_error(Lua, def, "Init", error);
      lua_pushinteger(Lua, int(error));
      release_object(def);
      return 1;
   }
   else {
      luaL_error(Lua, ERR::AccessObject);
      return 0;
   }
}

//********************************************************************************************************************
// __close metamethod handler for object auto-unlock.  Called automatically by scope exit for <close> variables.
// Safe no-op if accesscount is 0 (object was never locked or already unlocked).
//
// This feature ensure that locks acquired via `with` statements are always released, even if an exception occurs
// within the block.

static int object_close_handler(lua_State *Lua)
{
   auto *def = lj_get_object_fast(Lua, 1);
   if (def and def->accesscount > 0) release_object(def);
   return 0;
}

//********************************************************************************************************************
// __lock helper for `with` statement.  Validates the argument is an object, acquires the lock, and raises an error
// if the object is dead.

static int object_with_lock(lua_State *Lua)
{
   if (auto *def = lj_get_object_fast(Lua, 1)) {
      if (not access_object(def)) {
         luaL_error(Lua, ERR::AccessObject, "Failed to lock object for 'with' statement.");
         return 0;
      }
      lua_pushvalue(Lua, 1); // Return the object
      return 1;
   }
   else {
      luaL_argerror(Lua, 1, "Object expected for 'with' statement.");
      return 0;
   }
}

//********************************************************************************************************************

#include "lj_libdef.h"

extern "C" int luaopen_object(lua_State *L)
{
   LJ_LIB_REG(L, "obj", object);
   // Stack: [..., obj_lib_table]

   GCtab *lib = tabV(L->top - 1);
   global_State *g = G(L);

   // Add __index and __newindex metamethods directly to the library table.
   // These are called via the fallback path when BC_OBGETF/BC_OBSETF aren't used.
   lua_pushcfunction(L, object_index);
   lua_setfield(L, -2, "__index");

   lua_pushcfunction(L, object_newindex);
   lua_setfield(L, -2, "__newindex");

   lua_pushcfunction(L, object_close_handler);
   lua_setfield(L, -2, "__close");

   lua_pushcfunction(L, object_with_lock);
   lua_setfield(L, -2, "__lock");

   // Use the library table directly as the base metatable for objects.
   // NOBARRIER: basemt is a GC root.
   setgcref(basemt_it(g, LJ_TOBJECT), obj2gco(lib));

   // Register obj interface prototypes for compile-time type inference
   reg_iface_prototype("obj", "new", { TiriType::Object }, { TiriType::Str });
   reg_iface_prototype("obj", "find", { TiriType::Object }, { TiriType::Any });
   reg_iface_prototype("obj", "class", { TiriType::Object }, { TiriType::Object });
   reg_iface_prototype("obj", "init", { TiriType::Object }, { TiriType::Object });
   reg_iface_prototype("obj", "free", { TiriType::Nil }, { TiriType::Object });
   reg_iface_prototype("obj", "children", { TiriType::Table }, { TiriType::Object });
   reg_iface_prototype("obj", "detach", { TiriType::Object }, { TiriType::Object });
   reg_iface_prototype("obj", "get", { TiriType::Any }, { TiriType::Object, TiriType::Str });
   reg_iface_prototype("obj", "set", { TiriType::Object }, { TiriType::Object, TiriType::Str, TiriType::Any });
   reg_iface_prototype("obj", "getKey", { TiriType::Any }, { TiriType::Object, TiriType::Str });
   reg_iface_prototype("obj", "setKey", { TiriType::Object }, { TiriType::Object, TiriType::Str, TiriType::Any });
   reg_iface_prototype("obj", "delayCall", { TiriType::Nil }, { TiriType::Object, TiriType::Num, TiriType::Str }, FProtoFlags::Variadic);
   reg_iface_prototype("obj", "exists", { TiriType::Bool }, { TiriType::Any });
   reg_iface_prototype("obj", "subscribe", { TiriType::Object }, { TiriType::Object, TiriType::Str, TiriType::Func });
   reg_iface_prototype("obj", "unsubscribe", { TiriType::Object }, { TiriType::Object, TiriType::Any });

   return 1;
}
