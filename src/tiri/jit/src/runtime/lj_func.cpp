// Function handling (prototypes, functions and upvalues).
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
//
// Portions taken verbatim or adapted from the Lua interpreter.
// Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h

#define lj_func_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_func.h"
#include "lj_contract.h"
#include "lj_trace.h"
#include "lj_vm.h"

#ifdef UNIT_TESTS
ClosureAllocationProbe &lj_func_allocation_probe()
{
   static thread_local ClosureAllocationProbe probe;
   return probe;
}

static void func_probe_begin(lua_State *L, GCproto *Proto, int Helper)
{
   auto &probe = lj_func_allocation_probe();
   probe.state = L;
   probe.prototype = Proto;
   probe.helper = Helper;
   probe.site = -1;
   probe.active = true;
}
#define FUNC_PROBE_BEGIN(L, Proto, Helper) func_probe_begin(L, Proto, Helper)
#define FUNC_PROBE_SITE(Site) lj_func_allocation_probe().site = Site
#define FUNC_PROBE_END() lj_func_allocation_probe().active = false
#else
#define FUNC_PROBE_BEGIN(L, Proto, Helper) ((void)0)
#define FUNC_PROBE_SITE(Site) ((void)0)
#define FUNC_PROBE_END() ((void)0)
#endif

// Prototypes

void lj_func_freeproto(global_State *g, GCproto *pt)
{
   if (auto cache = proto_contract_cache(pt)) lj_mem_free(g, cache, cache->byte_size);
   if (auto map = pt->compilation_sources.get<CompilationSourceMap>()) {
      const MSize bytes = MSize(sizeof(CompilationSourceMap) + map->count * sizeof(CompilationSourceEntry));
      lj_mem_free(g, map, bytes);
   }

   // Free try-except metadata if present
   if (pt->try_blocks) lj_mem_free(g, pt->try_blocks, pt->try_block_count * sizeof(TryBlockDesc));
   if (pt->try_handlers) lj_mem_free(g, pt->try_handlers, pt->try_handler_count * sizeof(TryHandlerDesc));
   if (pt->context_blocks) {
      lj_mem_free(g, pt->context_blocks, pt->context_block_count * sizeof(ProtoContextBlockDesc));
   }

   // The resolved dependency sidecar holds non-owning pointers into the global module registry, so releasing it must
   // not touch the records themselves.  The registry outlives every Tiri state by contract (see expunge_modules()).

   if (pt->resolved_dependencies) {
      lj_mem_free(g, pt->resolved_dependencies, pt->resolved_count * sizeof(void *));
   }
   if (pt->resolved_dependency_states) {
      lj_mem_free(g, pt->resolved_dependency_states, pt->resolved_dependency_count * sizeof(uint8_t));
   }

   lj_mem_free(g, pt, pt->sizept);
}

// Upvalues

static void unlinkuv(global_State *g, GCupval *uv)
{
   lj_assertG(uvprev(uvnext(uv)) IS uv and uvnext(uvprev(uv)) IS uv, "broken upvalue chain");
   setgcrefr(uvnext(uv)->prev, uv->prev);
   setgcrefr(uvprev(uv)->next, uv->next);
}

// Find existing open upvalue for a stack slot or create a new one.

static GCupval * func_finduv(lua_State *L, TValue *slot)
{
   global_State* g = G(L);
   GCRef* pp = &L->openupval;
   GCupval* p;
   GCupval* uv;
   // Search the sorted list of open upvalues.
   while (gcref(*pp) != nullptr and uvval((p = gco_to_upval(gcref(*pp)))) >= slot) {
      lj_assertG(!p->closed and uvval(p) != &p->tv, "closed upvalue in chain");
      if (uvval(p) IS slot) {  // Found open upvalue pointing to same slot?
         if (isdead(g, obj2gco(p)))  //  Resurrect it, if it's dead.
            flipwhite(obj2gco(p));
         return p;
      }
      pp = &p->nextgc;
   }
   // No matching upvalue found. Create a new one.
   uv = lj_mem_newt(L, sizeof(GCupval), GCupval);
   newwhite(g, uv);
   uv->gct = ~LJ_TUPVAL;
   uv->closed = 0;  //  Still open.
   setmref(uv->v, slot);  //  Pointing to the stack slot.
   // NOBARRIER: The GCupval is new (marked white) and open.
   setgcrefr(uv->nextgc, *pp);  //  Insert into sorted list of open upvalues.
   setgcref(*pp, obj2gco(uv));
   setgcref(uv->prev, obj2gco(&g->uvhead));  //  Insert into GC list, too.
   setgcrefr(uv->next, g->uvhead.next);
   setgcref(uvnext(uv)->prev, obj2gco(uv));
   setgcref(g->uvhead.next, obj2gco(uv));
   lj_assertG(uvprev(uvnext(uv)) IS uv and uvnext(uvprev(uv)) IS uv, "broken upvalue chain");
   return uv;
}

// Create an empty and closed upvalue.

static GCupval* func_emptyuv(lua_State* L)
{
   GCupval* uv = (GCupval*)lj_mem_newgco(L, sizeof(GCupval));
   uv->gct = ~LJ_TUPVAL;
   uv->closed = 1;
   setnilV(&uv->tv);
   setmref(uv->v, &uv->tv);
   return uv;
}

// Close all open upvalues pointing to some stack level or above.

void lj_func_closeuv(lua_State *L, TValue *level)
{
   GCupval* uv;
   global_State* g = G(L);
   while (gcref(L->openupval) != nullptr and uvval((uv = gco_to_upval(gcref(L->openupval)))) >= level) {
      GCobj* o = obj2gco(uv);
      lj_assertG(!isblack(o), "bad black upvalue");
      lj_assertG(!uv->closed and uvval(uv) != &uv->tv, "closed upvalue in chain");
      setgcrefr(L->openupval, uv->nextgc);  //  No longer in open list.

      if (isdead(g, o)) lj_func_freeuv(g, uv);
      else {
         unlinkuv(g, uv);
         gc(g).closeUpvalue(uv);  // Phase 4 migration: use GarbageCollector facade
      }
   }
}

void lj_func_freeuv(global_State* g, GCupval* uv)
{
   if (!uv->closed) unlinkuv(g, uv);
   lj_mem_freet(g, uv);
}

// Functions (closures)

GCfunc * lj_func_newC(lua_State* L, MSize nelems, GCtab* env)
{
   auto fn = (GCfunc *)lj_mem_newgco(L, sizeCfunc(nelems));
   fn->c.gct = ~LJ_TFUNC;
   fn->c.ffid = FF_C;
   fn->c.nupvalues = (uint8_t)nelems;
   // NOBARRIER: The GCfunc is new (marked white).
   setmref(fn->c.pc, &G(L)->bc_cfunc_ext);
   setgcref(fn->c.env, obj2gco(env));
   return fn;
}

static GCfunc* func_newL(lua_State *L, GCproto *pt, GCtab *env)
{
   uint32_t count;
   auto fn = (GCfunc *)lj_mem_newgco(L, sizeLfunc((MSize)pt->sizeuv));
   fn->l.gct = ~LJ_TFUNC;
   fn->l.ffid = FF_LUA;
   fn->l.nupvalues = 0;  //  Set to zero until upvalues are initialized.
   // NOBARRIER: Really a setgcref. But the GCfunc is new (marked white).
   setmref(fn->l.pc, proto_bc(pt));
   setgcref(fn->l.env, obj2gco(env));
   // Saturating 3 bit counter (0..7) for created closures.
   count = (uint32_t)pt->flags + PROTO_CLCOUNT;
   pt->flags = (uint8_t)(count - ((count >> PROTO_CLC_BITS) & PROTO_CLCOUNT));
   return fn;
}

// Trace allocation must not collect or inspect the interpreter frame.  CALLA supplies GC checks and snapshots.

GCfunc *lj_func_newL_zero(lua_State *L, GCproto *Proto, GCtab *Environment)
{
   lj_assertL(Proto->sizeuv IS 0, "trace closure allocation with captures");
   FUNC_PROBE_BEGIN(L, Proto, 1);
   GCfunc *function = func_newL(L, Proto, Environment);
   FUNC_PROBE_END();
   return function;
}

// Share existing cells without collecting, creating open cells or inspecting the interpreter frame.

GCfunc *lj_func_newL_inherited(lua_State *L, GCproto *Proto, GCfuncL *Parent)
{
   FUNC_PROBE_BEGIN(L, Proto, 2);
   GCfunc *function = func_newL(L, Proto, tabref(Parent->env));
   for (MSize index = 0; index < Proto->sizeuv; ++index) {
      uint32_t capture = proto_uv(Proto)[index];
      lj_assertL(not (capture & PROTO_UV_LOCAL), "trace closure allocation with local capture");
      lj_assertL(capture < Parent->nupvalues, "invalid inherited capture index");
      // NOBARRIER: The function is new and white; preserve the cell's immutable flag and disambiguation hash.
      setgcrefr(function->l.uvptr[index], Parent->uvptr[capture]);
   }
   function->l.nupvalues = uint8_t(Proto->sizeuv);
   FUNC_PROBE_END();
   return function;
}

// Trace-local captures use the logical frame supplied by the recorder.  Neither allocation path collects or
// resizes the stack.  Create cells first so allocation failure never leaves a partially sized function in the GC list.

static GCfunc *func_newL_local(lua_State *L, GCproto *Proto, GCfuncL *Parent, TValue *Base)
{
   GCupval *captures[LJ_MAX_UPVAL];
   for (MSize index = 0; index < Proto->sizeuv; ++index) {
      uint32_t capture = proto_uv(Proto)[index];
      if (capture & PROTO_UV_LOCAL) {
         FUNC_PROBE_SITE(int(index));
         GCupval *cell = func_finduv(L, Base + (capture & 0xff));
         cell->immutable = ((capture / PROTO_UV_IMMUTABLE) & 1);
         cell->dhash = uint32_t(uintptr_t(mref<char>(Parent->pc))) ^ (capture << 24);
         captures[index] = cell;
      }
      else captures[index] = gco_to_upval(gcref(Parent->uvptr[capture]));
   }
   FUNC_PROBE_SITE(-1);
   GCfunc *function = func_newL(L, Proto, tabref(Parent->env));
   for (MSize index = 0; index < Proto->sizeuv; ++index) {
      // NOBARRIER: The function is white, and no allocation or collection intervenes before publication.
      setgcref(function->l.uvptr[index], obj2gco(captures[index]));
   }
   function->l.nupvalues = uint8_t(Proto->sizeuv);
   return function;
}

GCfunc *lj_func_newL_local(lua_State *L, GCproto *Proto, GCfuncL *Parent, TValue *Base)
{
   FUNC_PROBE_BEGIN(L, Proto, 3);
   GCfunc *function = func_newL_local(L, Proto, Parent, Base);
   FUNC_PROBE_END();
   return function;
}

// Create a new Lua function with empty upvalues.

GCfunc * lj_func_newL_empty(lua_State *L, GCproto *pt, GCtab *env)
{
   GCupval *captures[LJ_MAX_UPVAL];
   MSize i, nuv = pt->sizeuv;
   // These allocations do not collect. Build every cell before publishing the full-sized function.
   for (i = 0; i < nuv; i++) {
      GCupval *uv = func_emptyuv(L);
      int32_t v = proto_uv(pt)[i];
      uv->immutable = ((v / PROTO_UV_IMMUTABLE) & 1);
      uv->dhash = (uint32_t)(uintptr_t)pt ^ (v << 24);
      captures[i] = uv;
   }
   GCfunc *fn = func_newL(L, pt, env);
   // NOBARRIER: The GCfunc is new (marked white).
   for (i = 0; i < nuv; i++) setgcref(fn->l.uvptr[i], obj2gco(captures[i]));
   fn->l.nupvalues = (uint8_t)nuv;
   return fn;
}

// Do a GC check and create a new Lua function with inherited upvalues.

GCfunc * lj_func_newL_gc(lua_State *L, GCproto *pt, GCfuncL *parent)
{
   lj_gc_check_fixtop(L);
   // Resolve cells before publishing the function, as on trace.  A failed cell allocation must not leave a
   // full-sized function with zero nupvalues in the GC list (its destructor would free/account the wrong size).
   FUNC_PROBE_BEGIN(L, pt, 4);
   GCfunc *function = func_newL_local(L, pt, parent, L->base);
   FUNC_PROBE_END();
   return function;
}

void lj_func_free(global_State* g, GCfunc* fn)
{
   MSize size = isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) : sizeCfunc((MSize)fn->c.nupvalues);
   lj_mem_free(g, fn, size);
}
