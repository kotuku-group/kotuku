// Native struct handling.
// Copyright (C) 2026 Paul Manias

#define lj_struct_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_ir.h"
#include "lj_bc.h"
#include "lj_struct.h"
#include "lauxlib.h"

#include <cstring>
#include <string_view>
#include <kotuku/main.h>
#include <kotuku/modules/tiri.h>
#include "../../defs.h"

//********************************************************************************************************************
// Create a new GCstruct with an inline payload.  The payload follows the header, is zeroed, and any C++ string
// fields are default-constructed.  The allocation size must be mirrored by GCstruct::alloc_size() exactly, or the
// close_state leak assert fires.

GCstruct * lj_struct_new(lua_State *L, struct_record &Def)
{
   auto s = (GCstruct *)lj_mem_newgco(L, sizeof(GCstruct) + ALIGN64(size_t(Def.Size)));
   s->gct        = ~LJ_TSTRUCT;
   s->flags      = 0;
   s->unused1    = 0;
   s->structsize = uint32_t(Def.Size);
   s->data       = (void *)(s + 1);
   setgcrefnull(s->gclist);
   setgcrefnull(s->metatable);
   s->def        = &Def;
   s->lifecycle  = nullptr;
   setgcrefnull(s->parent);

   if (Def.Size > 0) std::memset(s->data, 0, size_t(Def.Size));
   construct_struct_cpp_strings(Def, s->data);
   return s;
}

//********************************************************************************************************************
// Create a GCstruct header that references externally managed memory.  Pass STRUCT_DEALLOCATE in Flags if the
// memory was allocated with AllocMemory() and ownership transfers to the GC.
//
// If the payload's validity depends on a Kotuku object staying alive (e.g. live views of object struct fields),
// pass that object as Lifecycle.  The view takes a weak pin so that the object's header remains testable after
// termination, and lj_struct_stale() reports the view as dead from that point onwards.  Weak pins never impede
// object termination (see the zombie contract in kotuku/objects.h).
//
// Parent optionally anchors the inline GCstruct that owns Data for interior embedded-struct views.

GCstruct * lj_struct_new_external(lua_State *L, struct_record &Def, void *Data, uint8_t Flags, Object *Lifecycle,
   GCstruct *Parent)
{
   auto s = (GCstruct *)lj_mem_newgco(L, sizeof(GCstruct));
   s->gct        = ~LJ_TSTRUCT;
   s->flags      = uint8_t(Flags | STRUCT_EXTERNAL);
   s->unused1    = 0;
   s->structsize = uint32_t(Def.Size);
   s->data       = Data;
   setgcrefnull(s->gclist);
   setgcrefnull(s->metatable);
   s->def        = &Def;
   s->lifecycle  = Lifecycle;
   if (Parent) setgcref(s->parent, obj2gco(Parent));
   else setgcrefnull(s->parent);
   if (Lifecycle) {
      Lifecycle->pinWeak();
      s->flags |= STRUCT_LIFECYCLE;
   }
   return s;
}

//********************************************************************************************************************
// Free a GCstruct during the GC sweep phase.  C++ string fields are destroyed only when the payload is owned
// (inline, or external with deallocation) - this mirrors the ownership condition of the legacy struct_destruct().

void lj_struct_free(global_State *g, GCstruct *s)
{
   // The main-thread lua_State shares the ~LJ_TSTRUCT gct but is LJ_GC_FIXED and never swept.
   lj_assertG(obj2gco(s) != obj2gco(mainthread(g)), "attempt to free main thread as struct");

   if (s->def and s->data and (s->is_deallocate() or (s->data IS (void *)(s + 1)))) {
      destroy_struct_cpp_strings(*s->def, s->data);
   }

   if (s->is_deallocate()) { FreeResource(s->data); s->data = nullptr; }

   // Releasing the last weak pin collects the zombie header if the object was destroyed while this view lived.
   if (s->is_lifecycle_bound()) {
      s->lifecycle->unpinWeak();
      s->lifecycle = nullptr;
   }

   lj_mem_free(g, s, s->alloc_size());
}

//********************************************************************************************************************
// Report whether a struct view has outlived the Kotuku object that owns its payload, poisoning the data pointer on
// detection so that later NULL-data checks also fail safely.
//
// This guard is specific to structs carrying a dependency on an object's lifecycle (STRUCT_LIFECYCLE); inline and
// plain external structs return false without touching the pin machinery.  For JIT-compiled accesses the same rule
// applies: the guard would be compiled out of the trace whenever the struct has no lifecycle dependency (structs
// are presently not traced - FD_STRUCT fields fall back to the interpreter - but any future fast path must emit
// this test as a trace guard for lifecycle-bound views only).
//
// The weak pin taken at creation guarantees the object header stays readable after termination (only Flags and
// RefCount remain valid at that point), which makes the terminating() test safe here.

bool lj_struct_stale(GCstruct *s)
{
   if (not s->is_lifecycle_bound()) return false;
   if (not s->lifecycle->terminating()) return false; // The lifecycle object is still pinned at this point
   s->data = nullptr;
   return true;
}

//********************************************************************************************************************
// Guard access to a struct view whose payload is owned by a Kotuku object.

void lj_struct_check_lifecycle(lua_State *L, GCstruct *Struct, const char *FieldName)
{
   if (lj_struct_stale(Struct)) {
      luaL_error(L, ERR::DoesNotExist, "Cannot access field '%s'; the object providing this struct has been destroyed.",
         FieldName);
   }
}

//********************************************************************************************************************
// Resolve a struct field and maintain the instruction's P32 inline cache.  Struct definitions are stable for the
// lifetime of the Lua state, but the hash is checked on every hit so polymorphic access sites self-heal.

static struct_field * find_cached_field(GCstruct *Struct, GCstr *Key, BCIns *Ins)
{
   if (not Struct->def) return nullptr;

   auto &fields = Struct->def->Fields;
   const auto field_hash = strihash(strdata(Key));
   if (Ins) {
      const uint32_t cached = bc_p32(*Ins);
      if ((cached != 0xFFFFFFFFu) and (cached < fields.size()) and
            (fields[cached].nameHash() IS field_hash)) return &fields[cached];
   }

   for (uint32_t i = 0; i < fields.size(); i++) {
      if (fields[i].nameHash() IS field_hash) {
         if (Ins) setbc_p32(Ins, i);
         return &fields[i];
      }
   }
   return nullptr;
}

//********************************************************************************************************************
// JIT field type lookup.  Struct definitions are immutable for the lifetime of the Lua state, so the recorder can
// derive a stable result type without reading the field payload or invoking any field access behaviour.

extern "C" int ir_struct_field_type(GCstruct *Struct, GCstr *Key, int &Offset, uint32_t &Flags)
{
   if (not Struct or not Key) return -1;

   if (auto field = find_cached_field(Struct, Key, nullptr)) {
      const uint32_t flags = uint32_t(field->Type);
      Offset = field->Offset;
      Flags = flags;

      // Order is significant because array, struct and object fields also carry element/storage flags.
      if ((flags & FD_CUSTOM) and (flags & FD_BYTE) and (flags & FD_ARRAY)) return IRT_STR;
      else if (flags & FD_ARRAY) return IRT_ARRAY;
      else if (flags & FD_STRING) return IRT_STR;
      else if (flags & FD_STRUCT) return IRT_STRUCT;
      else if (flags & FD_OBJECT) return IRT_OBJECT;
      else if (flags & (FD_POINTER|FD_FUNCTION)) return -1;
      else if (flags & (FD_DOUBLE|FD_FLOAT|FD_INT64)) return IRT_NUM;
      else if (flags & (FD_INT|FD_WORD|FD_BYTE)) return LJ_DUALNUM ? IRT_INT : IRT_NUM;
   }
   return -1;
}

//********************************************************************************************************************
// Fast struct field get - called from BC_STGETF.  The field core pushes its result and this helper copies it directly
// to Dest before restoring the interpreter's stack pointers.

extern "C" void bc_struct_getfield(lua_State *L, GCstruct *Struct, GCstr *Key, TValue *Dest, BCIns *Ins)
{
   const auto saved_base = L->base;
   const auto saved_top = L->top;

   if (not Ins) {
      auto jit_base = tvref(G(L)->jit_base);
      if (jit_base) L->base = jit_base;
   }
   if (curr_funcisL(L)) L->top = curr_topL(L);

   const auto field_name = strdata(Key);
   if (std::string_view("structSize") IS field_name) {
      lj_struct_push_size_closure(L, Struct);
      copyTV(L, Dest, L->top - 1);
      L->base = saved_base;
      L->top = saved_top;
      return;
   }

   lj_struct_check_lifecycle(L, Struct, field_name);
   if (not Struct->data) {
      luaL_error(L, ERR::Failed, "Cannot reference field '%s' because struct address is NULL.", field_name);
   }

   if (auto field = find_cached_field(Struct, Key, Ins)) {
      auto address = (int8_t *)Struct->data + field->Offset;
      lj_struct_getfield_core(L, Struct, *field, address);
      copyTV(L, Dest, L->top - 1);
      L->base = saved_base;
      L->top = saved_top;
      return;
   }

   luaL_error(L, ERR::FieldNotFound, "Field '%s' does not exist in structure.", field_name);
}

//********************************************************************************************************************
// Fast struct field set - called from BC_STSETF.  A protected copy of Val is placed at the top of the Lua stack before
// any operation can allocate or raise, and the shared field core consumes that value.

extern "C" void bc_struct_setfield(lua_State *L, GCstruct *Struct, GCstr *Key, TValue *Val, BCIns *Ins)
{
   const auto saved_base = L->base;
   const auto saved_top = L->top;

   if (not Ins) {
      auto jit_base = tvref(G(L)->jit_base);
      if (jit_base) L->base = jit_base;
   }
   if (curr_funcisL(L)) L->top = curr_topL(L);

   // Ensure L->top is past the value register before any error can be thrown. luaL_error pushes its error string at
   // L->top, so a stale top could overwrite the value or another active VM register.
   const auto stack_base = tvref(L->stack);
   const auto stack_end = stack_base + L->stacksize;
   auto val_ptr = Val;
   if ((Val < stack_base) or (Val >= stack_end)) {
      copyTV(L, L->top, Val);
      val_ptr = L->top;
      L->top++;
   }
   else if (L->top <= Val) L->top = Val + 1;

   // The shared setter core consumes its value from the stack top. Avoid a second copy when the protection step
   // already placed the value there.
   if (val_ptr != L->top - 1) {
      copyTV(L, L->top, val_ptr);
      L->top++;
   }

   const auto field_name = strdata(Key);
   lj_struct_check_lifecycle(L, Struct, field_name);
   if (not Struct->data) luaL_error(L, "Cannot reference field '%s' because struct address is NULL.", field_name);

   if (auto field = find_cached_field(Struct, Key, Ins)) {
      auto address = (int8_t *)Struct->data + field->Offset;
      lj_struct_setfield_core(L, Struct, *field, address);
      L->base = saved_base;
      L->top = saved_top;
      return;
   }

   luaL_error(L, "Invalid field reference '%s'", field_name);
}
