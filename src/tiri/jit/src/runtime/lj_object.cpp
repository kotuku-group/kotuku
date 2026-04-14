// Native Kotuku object handling for LuaJIT.
// Copyright (C) 2026 Paul Manias

#define lj_object_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_ir.h"
#include "lj_bc.h"
#include "lj_object.h"
#include "lj_str.h"
#include "../../defs.h"

//********************************************************************************************************************
// Create a new GCobject for a Kotuku object reference.  The object is allocated via the GC.

GCobject * lj_object_new(lua_State *L, OBJECTID UID, OBJECTPTR Ptr, objMetaClass *ClassPtr, uint8_t Flags)
{
   // lj_mem_newgco allocates and links to GC root list with newwhite()
   auto obj = (GCobject *)lj_mem_newgco(L, sizeof(GCobject));
   obj->gct         = ~LJ_TOBJECT;
   obj->udtype      = 0;
   obj->flags       = Flags;
   obj->accesscount = 0;
   setgcrefnull(obj->metatable);
   setgcrefnull(obj->gclist);
   obj->uid         = UID;
   obj->ptr         = Ptr;
   obj->classptr    = ClassPtr;

   return obj;
}

//********************************************************************************************************************
// Finalize a GCobject during GC finalization phase. This is called directly by the GC without metamethod lookup.
// Releases any locks and frees the underlying Kotuku object if owned by this script.

void lj_object_finalize(lua_State *L, GCobject *obj)
{
   while (obj->accesscount > 0) release_object(obj); // Critical for recovering from exceptions

   if (not obj->is_detached()) {
      // Only free the Kotuku object if it's owned by this script.
      // Exception: Recordset objects are always freed as they must be owned by a Database object.
      if (auto ptr = GetObjectPtr(obj->uid)) {
         if ((ptr->Class->BaseClassID IS CLASSID::RECORDSET) or
             (ptr->Owner IS L->script) or
             (ptr->ownerID() IS L->script->TargetID)) {
            pf::Log log("obj.destruct");
            log.traceBranch("Freeing Tiri-owned object #%d.", obj->uid);
            FreeResource(ptr);
         }
      }
   }
}

//********************************************************************************************************************
// Free a GCobject during garbage collection sweep phase.
// NOTE: The underlying Kotuku object is NOT freed here. It should have been freed earlier
// during the finalization phase by lj_object_finalize().
// This function only releases any remaining locks and frees the GCobject wrapper itself.

void lj_object_free(global_State *g, GCobject *obj)
{
   // Release any active locks before freeing the wrapper
   while (obj->accesscount > 0) {
      if (obj->flags & GCOBJ_LOCKED) {
         ReleaseObject((OBJECTPTR)obj->ptr);
         obj->flags &= ~GCOBJ_LOCKED;
         obj->ptr = nullptr;
      }
      obj->accesscount--;
   }

   // Free the GCobject structure (Kotuku object should have been freed by __gc finalizer)
   lj_mem_free(g, obj, sizeof(GCobject));
}

//********************************************************************************************************************
// pairs() iterator for objects - returns field name and flags for each iteration.

static int object_next_pair(lua_State *L)
{
   auto fields = (Field *)lua_touserdata(L, lua_upvalueindex(1));
   int field_total = lua_tointeger(L, lua_upvalueindex(2));
   int field_index = lua_tointeger(L, lua_upvalueindex(3));

   if ((field_index >= 0) and (field_index < field_total)) {
      lua_pushinteger(L, field_index + 1);
      lua_replace(L, lua_upvalueindex(3)); // Update the field counter

      lua_pushstring(L, fields[field_index].Name);
      lua_pushinteger(L, fields[field_index].Flags);
      return 2;
   }
   else return 0; // Terminates the iteration
}

int lj_object_pairs(lua_State *L)
{
   auto def = objectV(L->base);

   Field *fields;
   int total;
   if (def->classptr->get(FID_Dictionary, fields, total) IS ERR::Okay) {
      // Create the iterator closure with upvalues
      lua_pushlightuserdata(L, fields);
      lua_pushinteger(L, total);
      lua_pushinteger(L, 0);
      lua_pushcclosure(L, object_next_pair, 3);

      // FFH return values are placed at specific stack positions:
      // L->base - 2: Iterator function
      // L->base - 1: State (unused)
      // L->base:     Initial key (nil for pairs)
      TValue *o = L->base;
      copyTV(L, o - 2, L->top - 1);  // Copy closure to return position
      setnilV(o - 1);  // State (unused, closure uses upvalues)
      setnilV(o);      // Initial control variable
      L->top--;        // Pop the closure from top (now at FFH return position)
      return 3;
   }
   else luaL_error(L, ERR::FieldSearch, "Object class defines no fields.");
   return 0;
}

//********************************************************************************************************************
// ipairs() iterator for objects - returns field index and name for each iteration.

static int object_next_ipair(lua_State *L)
{
   auto fields = (Field *)lua_touserdata(L, lua_upvalueindex(1));
   int field_total = lua_tointeger(L, lua_upvalueindex(2));
   int field_index = lua_tointeger(L, 2); // Arg 2 is the previous index. It's nil if this is the first iteration.

   if ((field_index >= 0) and (field_index < field_total)) {
      lua_pushinteger(L, field_index + 1);
      lua_pushstring(L, fields[field_index].Name);
      return 2;
   }
   else return 0; // Terminates the iteration
}

int lj_object_ipairs(lua_State *L)
{
   auto def = objectV(L->base);

   Field *fields;
   int total;
   if (def->classptr->get(FID_Dictionary, fields, total) IS ERR::Okay) {
      // Create the iterator closure with upvalues
      lua_pushlightuserdata(L, fields);
      lua_pushinteger(L, total);
      lua_pushcclosure(L, object_next_ipair, 2);

      // FFH return values are placed at specific stack positions:
      // L->base - 2: Iterator function
      // L->base - 1: State (unused)
      // L->base:     Initial key (0 for ipairs)
      TValue *o = L->base;
      copyTV(L, o - 2, L->top - 1);  // Copy closure to return position
      setnilV(o - 1);  // State (unused, closure uses upvalues)
      setintV(o, 0);   // Initial control variable (field index starts at 0)
      L->top--;        // Pop the closure from top (now at FFH return position)
      return 3;
   }
   else luaL_error(L, ERR::FieldSearch, "Object class defines no fields.");
   return 0;
}

//********************************************************************************************************************
// Fast object field get - called from BC_OBGETF bytecode handler. Writes result directly to Dest, or throws an error
// if the field doesn't exist or the object has been freed.
//
// Ins points to the current 64-bit instruction.  The P32 field caches the read table index for O(1) repeat access.
// P32 = 0xFFFFFFFF means uncached.  Ins is nullptr when called from JIT traces (no caching).
//
// NOTE: Type-fixing rules insist that the referenced object is always the same class.  Polymorphic objects (whereby
// a class ID is not linked to the object type during parsing) cannot be JIT optimised.

extern "C" void bc_object_getfield(lua_State *L, GCobject *Obj, GCstr *Key, TValue *Dest, BCIns *Ins)
{
   // L->top is not maintained by the VM assembly between bytecodes.  It must be synchronised
   // before calling handlers that trigger lj_gc_check (e.g. push_object -> lua_pushobject),
   // because GC's atomic phase clears everything above L->top to nil — which would destroy
   // active stack slots like for-loop control variables.
   //
   // For JIT traces (Ins == nullptr), L->base is also stale; sync from jit_base first.

   const auto saved_base = L->base;
   const auto saved_top = L->top;

   if (not Ins) {
      auto jb = tvref(G(L)->jit_base);
      if (jb) L->base = jb;
   }

   if (curr_funcisL(L)) L->top = curr_topL(L);

   if (not Obj->uid) luaL_error(L, ERR::DoesNotExist, "Object dereferenced, unable to read field.");

   // Use raw pointers for std::lower_bound to avoid MSVC debug iterator tracking.
   // luaL_error() uses longjmp which skips C++ destructors, leaking debug iterator
   // registrations and corrupting the vector's proxy list on destruction.

   auto cl = Obj->classptr;
   auto read_table = get_read_table(cl);
   auto rt_data = read_table->data();
   auto rt_size = read_table->size();
   const obj_read *func;

   if (Ins) {
      uint32_t cached = bc_p32(*Ins);
      if ((cached != 0xFFFFFFFFu) and (cached < rt_size)
          and (rt_data[cached].Hash IS Key->hash)) {
         func = &rt_data[cached]; // Cache hit - O(1)
      }
      else { // Cache miss - binary search and cache
         auto found = std::lower_bound(rt_data, rt_data + rt_size, obj_read(Key->hash), read_hash);
         if ((found IS rt_data + rt_size) or (found->Hash != Key->hash)) {
            luaL_error(L, ERR::NoFieldAccess, "Field does not exist or is init-only: %s.%s", cl->ClassName, strdata(Key));
         }
         setbc_p32(Ins, uint32_t(found - rt_data));
         func = found;
      }
   }
   else { // JIT path - no caching
      auto found = std::lower_bound(rt_data, rt_data + rt_size, obj_read(Key->hash), read_hash);
      if ((found IS rt_data + rt_size) or (found->Hash != Key->hash)) {
         luaL_error(L, ERR::NoFieldAccess, "Field does not exist or is init-only: %s.%s", cl->ClassName, strdata(Key));
      }
      func = found;
   }

   // Call the field handler - it pushes result onto the Lua stack
   if (func->Call(L, *func, Obj) > 0) copyTV(L, Dest, L->top - 1);
   else { // An error occurred and is stored in L->CaughtError
      if (L->CaughtError > ERR::ExceptionThreshold) {
         luaL_error(L, L->CaughtError, "Read failure: %s.%s: %s", cl->ClassName, luaL_checkstring(L, 2), GetErrorMsg(L->CaughtError));
      }

      setnilV(Dest);
   }
   L->base = saved_base;
   L->top = saved_top;
}

//********************************************************************************************************************
// Fast object field set - called from BC_OBSETF bytecode handler. Writes Val to the object field, or throws an error.
//
// Ins points to the current 64-bit instruction.  The P32 field caches the write table index for O(1) repeat access.
// P32 = 0xFFFFFFFF means uncached.  Ins is nullptr when called from JIT traces (no caching).
//
// NOTE: Type-fixing rules insist that the referenced object is always the same class.  Polymorphic objects (whereby
// a class ID is not linked to the object type during parsing) cannot be JIT optimised.

extern "C" void bc_object_setfield(lua_State *L, GCobject *Obj, GCstr *Key, TValue *Val, BCIns *Ins)
{
   // L->top is not maintained by the VM assembly between bytecodes (see bc_object_getfield).
   // For JIT traces (Ins == nullptr), L->base is also stale; sync from jit_base first.

   const auto saved_base = L->base;
   const auto saved_top  = L->top;
   if (not Ins) {
      auto jb = tvref(G(L)->jit_base);
      if (jb) L->base = jb;
   }
   if (curr_funcisL(L)) L->top = curr_topL(L);

   // Ensure L->top is past the value register before any error can be thrown.
   // luaL_error pushes the error string to L->top, which would corrupt active registers if too low.
   const auto stack_base = tvref(L->stack);
   const auto stack_end  = stack_base + L->stacksize;
   auto val_ptr    = Val;
   if ((Val < stack_base) or (Val >= stack_end)) {
      copyTV(L, L->top, Val);
      val_ptr = L->top;
      L->top++;
   }
   else if (L->top <= Val) L->top = Val + 1;

   if (not Obj->uid) luaL_error(L, ERR::DoesNotExist, "Object dereferenced, unable to write field.");

   auto write_table = get_write_table(Obj->classptr);
   auto wt_data = write_table->data();
   auto wt_size = write_table->size();
   const obj_write *func;

   if (Ins) {
      uint32_t cached = bc_p32(*Ins);
      if ((cached != 0xFFFFFFFFu) and (cached < wt_size) and (wt_data[cached].Hash IS Key->hash)) {
         func = &wt_data[cached]; // Cache hit - O(1)
      }
      else { // Cache miss - binary search and cache
         auto found = std::lower_bound(wt_data, wt_data + wt_size, obj_write(Key->hash), write_hash);
         if ((found IS wt_data + wt_size) or (found->Hash != Key->hash)) {
            luaL_error(L, ERR::UndefinedField, "Field does not exist or is read-only: %s.%s",
               Obj->classptr ? Obj->classptr->ClassName : "?", strdata(Key));
         }
         setbc_p32(Ins, uint32_t(found - wt_data));
         func = found;
      }
   }
   else { // JIT path - no caching
      auto found = std::lower_bound(wt_data, wt_data + wt_size, obj_write(Key->hash), write_hash);
      if ((found IS wt_data + wt_size) or (found->Hash != Key->hash)) {
         luaL_error(L, ERR::UndefinedField, "Field does not exist or is read-only: %s.%s",
            Obj->classptr ? Obj->classptr->ClassName : "?", strdata(Key));
      }
      func = found;
   }

   if (auto pobj = access_object(Obj)) {
      auto stack_idx = int(val_ptr - L->base) + 1;
      ERR error = func->Call(L, pobj, func->Field, stack_idx);
      L->base = saved_base;
      L->top = saved_top;
      release_object(Obj);

      if (error >= ERR::ExceptionThreshold) luaL_error(L, error);
   }
   else luaL_error(L, ERR::AccessObject);
}

//********************************************************************************************************************
// JIT field type lookup - returns the IR type for a field.  Returns -1 if field not found or unknown type.  This
// function must have no side effects as it is called during JIT recording.

extern "C" int ir_object_field_type(GCobject *Obj, GCstr *Key, int &Offset, uint32_t &FieldFlags)
{
   if (not Obj->uid or not Obj->classptr) return -1;

   objMetaClass *src_class;
   Field *field;
   if (Obj->classptr->findField(Key->hash, &field, &src_class) IS ERR::Okay) {
      auto flags = field->Flags;
      if (not (flags & FDF_R)) return -1;  // Not readable

      if (field->GetValue) Offset = 0; // Default offset for fields that aren't directly accessible.
      else Offset = field->Offset; // Direct access is permitted.

      FieldFlags = flags;

      // NB: Order is important
      if (flags & FD_ARRAY) return IRT_ARRAY;
      else if (flags & FD_STRING) { FieldFlags &= ~FD_POINTER; return IRT_STR; }
      else if (flags & (FD_DOUBLE|FD_INT64)) return IRT_NUM;
      else if (flags & (FD_OBJECT|FD_LOCAL)) return IRT_OBJECT;
      else if (flags & FD_INT) {
         // FD_UNSIGNED always uses lua_pushnumber, even in DUALNUM builds
         if (flags & FD_UNSIGNED) return IRT_NUM;
         else return LJ_DUALNUM ? IRT_INT : IRT_NUM;
      }
      else if (flags & FD_STRUCT) {
         if (flags & FD_RESOURCE) return IRT_LIGHTUD;
         else return IRT_TAB;
      }
      else if (flags & FD_POINTER) return IRT_LIGHTUD;
      else return -1;  // Unknown type
   }
   else return -1;  // Field not found in dictionary
}

//********************************************************************************************************************
// JIT write-side field type lookup.  Returns the IR type for a writable numeric field, or -1 if the field is not
// found, not writable, or not a simple numeric type.  Sets Offset to the field's byte offset when direct memory
// writes are safe (SetValue == nullptr), or 0 when a virtual setter must be called.
// This function must have no side effects as it is called during JIT recording.

extern "C" int ir_object_field_type_write(GCobject *Obj, GCstr *Key, int &Offset, uint32_t &FieldFlags)
{
   if (not Obj->uid or not Obj->classptr) return -1;

   objMetaClass *src_class;
   Field *field;
   if (Obj->classptr->findField(Key->hash, &field, &src_class) IS ERR::Okay) {
      auto flags = field->Flags;
      if (not (flags & FD_WRITE)) return -1;           // Not writable (FD_INIT excluded)
      if (flags & (FD_FLAGS|FD_LOOKUP)) return -1;     // Special write handlers, not simple stores
      Offset = field->SetValue ? 0 : field->Offset;
      FieldFlags = flags;
      if (flags & (FD_DOUBLE|FD_INT64)) return IRT_NUM;
      else if (flags & FD_INT) {
         if (flags & FD_UNSIGNED) return IRT_NUM;
         else return LJ_DUALNUM ? IRT_INT : IRT_NUM;
      }
      else return -1;  // Non-numeric — not supported for write optimisation
   }
   else return -1;
}

//********************************************************************************************************************
// JIT fast-path lock: guards in the trace ensure the object is alive, non-detached, and has a valid ptr.
// Mirrors access_object() semantics: skips ptr->lock() if already held (accesscount > 0).
// Returns the Object* pointer for use by XLOAD.

extern "C" OBJECTPTR jit_object_lock(GCobject *Obj)
{
   if (not Obj->accesscount) Obj->ptr->lock();
   Obj->accesscount++;
   return Obj->ptr;
}

//********************************************************************************************************************
// JIT fast-path unlock: mirrors release_object() semantics for non-detached objects.

extern "C" void jit_object_unlock(GCobject *Obj)
{
   if (--Obj->accesscount IS 0) Obj->ptr->unlock();
}

//********************************************************************************************************************
// JIT fast-path string field read: locks the object, reads the CSTRING pointer at the given offset,
// unlocks, and writes the result to Out.  Null CSTRING values produce nil (matching lua_pushstring).
// Guards in the trace ensure the object is alive, non-detached, and has a valid ptr.

extern "C" void jit_object_getstr(lua_State *L, GCobject *Obj, uint32_t Offset, TValue *Out)
{
   if (not Obj->accesscount) Obj->ptr->lock();
   Obj->accesscount++;
   CSTRING str = *(CSTRING *)(((int8_t *)Obj->ptr) + Offset);
   if (--Obj->accesscount IS 0) Obj->ptr->unlock();
   if (str) setstrV(L, Out, lj_str_newz(L, str));
   else setnilV(Out); // Alternatively for an empty string: setstrV(L, Out, &G(L)->strempty);
}

//********************************************************************************************************************
// JIT fast-path object field read: locks the parent, reads the OBJECTPTR at the given offset, unlocks, and creates
// a detached GCobject wrapper written to Out.  Null pointers produce nil.  load_include_for_class() is not called
// because the interpreter will have already loaded the class definitions during prior execution cycles.
// Guards in the trace ensure the parent object is alive, non-detached, and has a valid ptr.

extern "C" void jit_object_getobj(lua_State *L, GCobject *Obj, uint32_t Offset, TValue *Out)
{
   if (not Obj->accesscount) Obj->ptr->lock();
   Obj->accesscount++;
   OBJECTPTR child = *(OBJECTPTR *)(((int8_t *)Obj->ptr) + Offset);
   if (child) {
      auto gcobj = lj_object_new(L, child->UID, nullptr, child->Class, GCOBJ_DETACHED);
      setobjectV(L, Out, gcobj);
   }
   else setnilV(Out);
   if (--Obj->accesscount IS 0) Obj->ptr->unlock();
}
