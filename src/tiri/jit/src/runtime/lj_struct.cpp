// Native struct handling.
// Copyright (C) 2026 Paul Manias

#define lj_struct_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_struct.h"

#include <cstring>
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
// NB: External structs referencing a parent's inline payload (embedded sub-structs) do not anchor the parent;
// the caller must keep the parent alive for the lifetime of the view.

GCstruct * lj_struct_new_external(lua_State *L, struct_record &Def, void *Data, uint8_t Flags, Object *Lifecycle)
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
