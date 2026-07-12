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

   if (Def.Size > 0) std::memset(s->data, 0, size_t(Def.Size));
   construct_struct_cpp_strings(Def, s->data);
   return s;
}

//********************************************************************************************************************
// Create a GCstruct header that references externally managed memory.  Pass STRUCT_DEALLOCATE in Flags if the
// memory was allocated with AllocMemory() and ownership transfers to the GC.
//
// NB: External structs referencing a parent's inline payload (embedded sub-structs) do not anchor the parent;
// the caller must keep the parent alive for the lifetime of the view.

GCstruct * lj_struct_new_external(lua_State *L, struct_record &Def, void *Data, uint8_t Flags)
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

   lj_mem_free(g, s, s->alloc_size());
}
