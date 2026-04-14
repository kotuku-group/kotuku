/*
** Garbage collector.
** Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
**
** Algorithm Overview:
** Implements a tri-colour incremental mark-and-sweep collector:
** - White: Unmarked objects (candidates for collection)
** - Grey: Marked but children not yet traversed
** - Black: Marked and all children traversed
**
** GC Phases (in order):
** 1. Pause     - Idle, waiting to start a new cycle
** 2. Propagate - Incrementally marking grey objects black
** 3. Atomic    - Non-interruptible transition from mark to sweep
** 4. SweepStr  - Sweeping string interning table
** 5. Sweep     - Sweeping main object list
** 6. Finalize  - Running __gc metamethods
**
** Write Barriers:
** - Forward barrier: When storing white object in black object during propagate
** - Backward barrier: When storing to black table (makes table grey again)
**
** See also: lj_gc.h for the public API and GarbageCollector facade class.
*/

#define lj_gc_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_udata.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#include "lj_trace.h"
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "lj_array.h"
#include "lj_object.h"
#include <kotuku/main.h>

#include <array>
#include <span>

// -- GC Configuration Constants -----------------------------------------------
// These control the incremental GC stepping behaviour.

static constexpr uint32_t GCSTEPSIZE     = 1024u;  // Base step size in bytes
static constexpr uint32_t GCSWEEPMAX     = 40;     // Max objects to sweep per step
static constexpr uint32_t GCSWEEPCOST    = 10;     // Cost estimate per sweep operation
static constexpr uint32_t GCFINALIZECOST = 100;    // Cost estimate per finaliser call

static void gc_mark(global_State *, GCobj *);
static void gc_traverse_array(global_State *, GCarray *);
static GCRef* gc_sweep(global_State *, GCRef *, uint32_t);

// The current trace is a GC root while not anchored in the prototype (yet).

#define gc_traverse_curtrace(g)   gc_traverse_trace(g, &G2J(g)->cur)

//********************************************************************************************************************
// RAII guard for GC finaliser state preservation.
// Saves hook state and GC threshold on construction, restores on destruction.
// Used during __gc metamethod calls to prevent re-entrant GC and hooks.

class GCFinaliserGuard {
   global_State *g_;
   uint8_t savedHook_;
   GCSize savedThreshold_;

public:
   explicit GCFinaliserGuard(global_State *g) noexcept
      : g_(g), savedHook_(hook_save(g)), savedThreshold_(g->gc.threshold)
   {
      hook_entergc(g);  // Disable hooks and new traces during __gc.
      g->gc.threshold = LJ_MAX_MEM;  // Prevent GC steps.
   }

   ~GCFinaliserGuard() noexcept {
      hook_restore(g_, savedHook_);
      g_->gc.threshold = savedThreshold_;
   }

   // Query saved hook state (needed for profile dispatch updates).
   [[nodiscard]] uint8_t savedHook() const noexcept { return savedHook_; }

   // Non-copyable, non-movable.
   GCFinaliserGuard(const GCFinaliserGuard&) = delete;
   GCFinaliserGuard& operator=(const GCFinaliserGuard&) = delete;
};

//********************************************************************************************************************
// RAII guard for VM state during GC operations.  Saves current VM state and sets it to GC mode on construction.
// Restores original state on destruction.

class VMStateGuard {
   global_State *g_;
   int32_t savedState_;

public:
   explicit VMStateGuard(global_State *g) noexcept : g_(g), savedState_(g->vmstate) {
      setvmstate(g, GC);
   }

   ~VMStateGuard() noexcept {
      g_->vmstate = savedState_;
   }

   // Non-copyable, non-movable.
   VMStateGuard(const VMStateGuard&) = delete;
   VMStateGuard& operator=(const VMStateGuard&) = delete;
};

//********************************************************************************************************************
// Inline Functions for GC Object Colour Manipulation

// Transition object from white to grey (marked but children not yet traversed).

inline void white2gray(GCobj *x) noexcept { x->gch.marked &= uint8_t(~LJ_GC_WHITES); }

// Transition object from grey to black (marked and all children traversed).

inline void gray2black(GCobj *x) noexcept { x->gch.marked |= LJ_GC_BLACK; }

// Check if userdata has been finalised.

[[nodiscard]] inline bool isfinalized(const GCudata* u) noexcept { return (u->marked & LJ_GC_FINALIZED) != 0; }

// Check if GCobject has been finalised.

[[nodiscard]] inline bool isfinalized(const GCobject* o) noexcept { return (o->marked & LJ_GC_FINALIZED) != 0; }

// Mark a string object (strings go directly to black, never grey).

inline void gc_mark_str(GCstr* s) noexcept { s->marked &= uint8_t(~LJ_GC_WHITES); }

//********************************************************************************************************************
// Mark a TValue if it contains a white GC object.

inline void gc_marktv(global_State *g, cTValue *tv) noexcept {
   lj_assertG(not tvisgcv(tv) or (~itype(tv) IS gcval(tv)->gch.gct), "TValue and GC type mismatch");
   if (tviswhite(tv)) gc_mark(g, gcV(tv));
}

//********************************************************************************************************************
// Mark a GC object if it is white.
// Uses C++20 concept to ensure T is a valid GC object type.

template<GCObjectType T>
inline void gc_markobj(global_State *g, T* o) noexcept {
   if (iswhite(obj2gco(o))) gc_mark(g, obj2gco(o));
}

//********************************************************************************************************************
// Mark a white GCobj.

static void gc_mark(global_State *g, GCobj* o)
{
   int gct = o->gch.gct;

   lj_assertG(iswhite(o), "mark of non-white object");
   lj_assertG(not isdead(g, o), "mark of dead object");

   white2gray(o);

   if (gct IS ~LJ_TUDATA) {
      GCtab* mt = tabref(gco_to_userdata(o)->metatable);
      gray2black(o);  //  Userdata are never gray.
      if (mt) gc_markobj(g, mt);
      gc_markobj(g, tabref(gco_to_userdata(o)->env));
      if (gco_to_userdata(o)->udtype IS UDTYPE_THUNK) {
         // Mark thunk payload contents to prevent GC from collecting them

         ThunkPayload *payload = thunk_payload(gco_to_userdata(o));

         // Mark the deferred function

         if (gcref(payload->deferred_func)) gc_markobj(g, gcref(payload->deferred_func));

         // Mark the cached value if it's a GC object

         if (payload->resolved and tvisgcv(&payload->cached_value)) {
            gc_markobj(g, gcval(&payload->cached_value));
         }
      }
   }
   else if (gct IS ~LJ_TUPVAL) {
      GCupval *uv = gco_to_upval(o);
      gc_marktv(g, uvval(uv));
      if (uv->closed) gray2black(o);  //  Closed upvalues are never gray.
   }
   else if (gct IS ~LJ_TARRAY) {
      GCarray *arr = gco_to_array(o);
      gray2black(o);  //  Arrays are never gray.
      gc_traverse_array(g, arr);
   }
   else if (gct IS ~LJ_TOBJECT) { // Native Kotuku object type - has no child GC references except optional metatable
      GCobject *obj = gco_to_object(o);
      gray2black(o);  // Objects are never gray (like userdata)

      // Mark metatable if present
      GCtab *mt = tabref(obj->metatable);
      if (mt) gc_markobj(g, mt);
   }
   else if (gct != ~LJ_TSTR) {
      lj_assertG(gct IS ~LJ_TFUNC or gct IS ~LJ_TTAB or
         gct IS ~LJ_TTHREAD or gct IS ~LJ_TPROTO or gct IS ~LJ_TTRACE, "bad GC type %d", gct);
      setgcrefr(o->gch.gclist, g->gc.gray);
      setgcref(g->gc.gray, o);
   }
}

//********************************************************************************************************************
// Mark GC roots.

static void gc_mark_gcroot(global_State *g)
{
   ptrdiff_t i;
   for (i = 0; i < GCROOT_MAX; i++) {
      if (gcref(g->gcroot[i]) != nullptr) gc_markobj(g, gcref(g->gcroot[i]));
   }
}

//********************************************************************************************************************
// Start a GC cycle and mark the root set.

static void gc_mark_start(global_State *g)
{
   setgcrefnull(g->gc.gray);
   setgcrefnull(g->gc.grayagain);
   setgcrefnull(g->gc.weak);
   gc_markobj(g, mainthread(g));
   gc_markobj(g, tabref(mainthread(g)->env));
   gc_marktv(g, &g->registrytv);
   gc_mark_gcroot(g);
   g->gc.state = (GCPhase::Propagate);
}

//********************************************************************************************************************
// Mark open upvalues.

static void gc_mark_uv(global_State *g)
{
   GCupval* uv;
   for (uv = uvnext(&g->uvhead); uv != &g->uvhead; uv = uvnext(uv)) {
      lj_assertG(uvprev(uvnext(uv)) IS uv and uvnext(uvprev(uv)) IS uv, "broken upvalue chain");
      if (isgray(obj2gco(uv))) gc_marktv(g, uvval(uv));
   }
}

//********************************************************************************************************************
// Mark userdata in mmudata list.

static void gc_mark_mmudata(global_State *g)
{
   GCobj *root = gcref(g->gc.mmudata);

   if (GCobj *u = root) {
      do {
         u = gcnext(u);
         makewhite(g, u);  //  Could be from previous GC.
         gc_mark(g, u);
      } while (u != root);
   }
}

//********************************************************************************************************************
// Helper to move an object to the mmudata finalization list.

static void gc_move_to_mmudata(global_State *g, GCobj *o, GCRef *p)
{
   markfinalized(o);
   *p = o->gch.nextgc;
   if (gcref(g->gc.mmudata)) {  // Link to end of mmudata list.
      GCobj *root = gcref(g->gc.mmudata);
      setgcrefr(o->gch.nextgc, root->gch.nextgc);
      setgcref(root->gch.nextgc, o);
      setgcref(g->gc.mmudata, o);
   }
   else {  // Create circular list.
      setgcref(o->gch.nextgc, o);
      setgcref(g->gc.mmudata, o);
   }
}

//********************************************************************************************************************
// Separate userdata objects to be finalized to mmudata list.

size_t lj_gc_separateudata(global_State *g, int all)
{
   size_t m = 0;
   GCRef *p = &mainthread(g)->nextgc;
   GCobj *o;
   while ((o = gcref(*p)) != nullptr) {
      if (not (iswhite(o) or all) or isfinalized(gco_to_userdata(o))) {
         p = &o->gch.nextgc;  //  Nothing to do.
      }
      else if (not lj_meta_fastg(g, tabref(gco_to_userdata(o)->metatable), MM_gc)) {
         markfinalized(o);  //  Done, as there's no __gc metamethod.
         p = &o->gch.nextgc;
      }
      else {  // Otherwise move userdata to be finalized to mmudata list.
         m += sizeudata(gco_to_userdata(o));
         gc_move_to_mmudata(g, o, p);
      }
   }
   return m;
}

//********************************************************************************************************************
// Separate GCobject (native Kotuku objects) to be finalized to mmudata list.
// All GCobject instances require finalization via lj_object_finalize() to free the Kotuku object.

static size_t gc_separateobjects(global_State *g, int all)
{
   size_t m = 0;
   GCRef *p = &g->gc.root;
   GCobj *o;

   while ((o = gcref(*p)) != nullptr) {
      if (o->gch.gct IS ~LJ_TOBJECT) {
         GCobject *obj = gco_to_object(o);
         if (not (iswhite(o) or all) or isfinalized(obj)) {
            p = &o->gch.nextgc;  //  Nothing to do.
         }
         else {
            // Move GCobject to mmudata list for finalization.
            m += sizeof(GCobject);
            gc_move_to_mmudata(g, o, p);
         }
      }
      else p = &o->gch.nextgc;
   }
   return m;
}

//********************************************************************************************************************
// Traverse a typed array.  Marks the metatable and all GC-reference elements.

static void gc_traverse_array(global_State *g, GCarray *Arr)
{
   GCtab *mt = tabref(Arr->metatable);
   if (mt) gc_markobj(g, mt);

   if (Arr->elemtype IS AET::STR_GC or Arr->elemtype IS AET::TABLE or Arr->elemtype IS AET::ARRAY) {
      GCRef *refs = Arr->get<GCRef>();
      for (MSize i = 0; i < Arr->len; i++) {
         if (gcref(refs[i])) gc_markobj(g, gcref(refs[i]));
      }
   }
   else if (Arr->elemtype IS AET::ANY) {
      TValue *slots = Arr->get<TValue>();
      for (MSize i = 0; i < Arr->len; i++) {
         gc_marktv(g, &slots[i]);
      }
   }
}

//********************************************************************************************************************
// Traverse a table.

static int gc_traverse_tab(global_State *g, GCtab* t)
{
   int weak = 0;
   cTValue *mode;
   GCtab *mt = tabref(t->metatable);

   if (mt) gc_markobj(g, mt);

   mode = lj_meta_fastg(g, mt, MM_mode);
   if (mode and tvisstr(mode)) {  // Valid __mode field?
      const char* modestr = strVdata(mode);
      int c;
      while ((c = *modestr++)) {
         if (c IS 'k') weak |= LJ_GC_WEAKKEY;
         else if (c IS 'v') weak |= LJ_GC_WEAKVAL;
      }

      if (weak) {  // Weak tables are cleared in the atomic phase.
         {
            t->marked = (uint8_t)((t->marked & ~LJ_GC_WEAK) | weak);
            setgcrefr(t->gclist, g->gc.weak);
            setgcref(g->gc.weak, obj2gco(t));
         }
      }
   }

   if (weak IS LJ_GC_WEAK) return 1; //  Nothing to mark if both keys/values are weak.

   // Mark array part (TValue has alignment attributes incompatible with std::span).

   if (not (weak & LJ_GC_WEAKVAL) and t->asize > 0) {
      TValue *array_start = arrayslot(t, 0);
      for (MSize i = 0; i < t->asize; i++) gc_marktv(g, &array_start[i]);
   }

   // Mark hash part using std::span for cleaner iteration.
   if (t->hmask > 0) {
      std::span<Node> hash_part(noderef(t->node), t->hmask + 1);
      for (Node& n : hash_part) {
         if (not tvisnil(&n.val)) {  // Mark non-empty slot.
            lj_assertG(not tvisnil(&n.key), "mark of nil key in non-empty slot");
            if (not (weak & LJ_GC_WEAKKEY)) gc_marktv(g, &n.key);
            if (not (weak & LJ_GC_WEAKVAL)) gc_marktv(g, &n.val);
         }
      }
   }
   return weak;
}

//********************************************************************************************************************
// Traverse a function.

static void gc_traverse_func(global_State *g, GCfunc* fn)
{
   gc_markobj(g, tabref(fn->c.env));
   if (isluafunc(fn)) {
      uint32_t i;
      lj_assertG(fn->l.nupvalues <= funcproto(fn)->sizeuv, "function upvalues out of range");
      gc_markobj(g, funcproto(fn));
      for (i = 0; i < fn->l.nupvalues; i++)  //  Mark Lua function upvalues.
         gc_markobj(g, &gcref(fn->l.uvptr[i])->uv);
   }
   else {
      uint32_t i;
      for (i = 0; i < fn->c.nupvalues; i++)  //  Mark C function upvalues.
         gc_marktv(g, &fn->c.upvalue[i]);
   }
}

//********************************************************************************************************************
// Mark a trace.

static void gc_marktrace(global_State *g, TraceNo traceno)
{
   GCobj* o = obj2gco(traceref(G2J(g), traceno));
   lj_assertG(traceno != G2J(g)->cur.traceno, "active trace escaped");
   if (iswhite(o)) {
      white2gray(o);
      setgcrefr(o->gch.gclist, g->gc.gray);
      setgcref(g->gc.gray, o);
   }
}

//********************************************************************************************************************
// Traverse a trace.

static void gc_traverse_trace(global_State *g, GCtrace* T)
{
   IRRef ref;
   if (T->traceno IS 0) return;
   for (ref = T->nk; ref < REF_TRUE; ref++) {
      IRIns* ir = &T->ir[ref];
      if (ir->o IS IR_KGC) gc_markobj(g, ir_kgc(ir));
      if (irt_is64(ir->t) and ir->o != IR_KNULL) ref++;
   }
   if (T->link) gc_marktrace(g, T->link);
   if (T->nextroot) gc_marktrace(g, T->nextroot);
   if (T->nextside) gc_marktrace(g, T->nextside);
   gc_markobj(g, gcref(T->startpt));
}

//********************************************************************************************************************
// Traverse a prototype.

static void gc_traverse_proto(global_State *g, GCproto* pt)
{
   ptrdiff_t i;
   gc_mark_str(proto_chunk_name(pt));
   for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)  //  Mark collectable consts.
      gc_markobj(g, proto_kgc(pt, i));
   if (pt->trace) gc_marktrace(g, pt->trace);
}

//********************************************************************************************************************
// Traverse the frame structure of a stack.

static MSize gc_traverse_frames(global_State *g, lua_State* th)
{
   TValue* frame, * top = th->top - 1, * bot = tvref(th->stack);

   // Sanity checks for stack state integrity.
   // These catch issues like VM helper functions being called without proper
   // stack synchronisation (e.g., L->top not set by VM assembler code).
   // See VMHelperGuard in stack_helpers.h for the proper fix pattern.
   lj_assertG(th->base >= bot, "stack base before stack start");
   lj_assertG(th->top >= th->base, "stack top before base - VM helper may need VMHelperGuard");
   lj_assertG(th->top <= tvref(th->maxstack), "stack overflow detected");
   lj_assertG(th->base <= tvref(th->maxstack), "stack base beyond maxstack");

   // Note: extra vararg frame not skipped, marks function twice (harmless).

   for (frame = th->base - 1; frame > bot + LJ_FR2; frame = frame_prev(frame)) {
      GCfunc *fn = frame_func(frame);

      // Validate function pointer before dereferencing
      lj_assertG(fn != nullptr, "null function in frame");
      lj_assertG(fn->c.gct IS ~LJ_TFUNC, "invalid function type in frame: %d", fn->c.gct);

      TValue* ftop = frame;
      if (isluafunc(fn)) ftop += funcproto(fn)->framesize;
      if (ftop > top) top = ftop;
      if (not LJ_FR2) gc_markobj(g, fn);  //  Need to mark hidden function (or L).
   }
   top++;  //  Correct bias of -1 (frame IS base-1).
   if (top > tvref(th->maxstack)) top = tvref(th->maxstack);
   return (MSize)(top - bot);  //  Return minimum needed stack size.
}

//********************************************************************************************************************
// Traverse a thread object.

static void gc_traverse_thread(global_State *g, lua_State* th)
{
   TValue *o, *top = th->top;
   for (o = tvref(th->stack) + 1 + LJ_FR2; o < top; o++) gc_marktv(g, o);
   if (g->gc.state IS (GCPhase::Atomic)) {
      top = tvref(th->stack) + th->stacksize;
      for (; o < top; o++)  //  Clear unmarked slots.
         setnilV(o);
   }
   gc_markobj(g, tabref(th->env));
   if (th->pending_trace) {
      CapturedStackTrace *trace = th->pending_trace;
      for (uint16_t i = 0; i < trace->frame_count; i++) {
         CapturedFrame *cf = &trace->frames[i];
         if (cf->source) gc_markobj(g, cf->source);
         if (cf->funcname) gc_markobj(g, cf->funcname);
      }
   }
   lj_state_shrinkstack(th, gc_traverse_frames(g, th));
}

//********************************************************************************************************************
// Propagate one gray object. Traverse it and turn it black.

static size_t propagatemark(global_State *g)
{
   GCobj* o = gcref(g->gc.gray);
   int gct = o->gch.gct;
   lj_assertG(isgray(o), "propagation of non-gray object");
   gray2black(o);
   setgcrefr(g->gc.gray, o->gch.gclist);  //  Remove from gray list.

   if (gct IS ~LJ_TTAB) [[likely]] {
      GCtab* t = gco_to_table(o);
      if (gc_traverse_tab(g, t) > 0) black2gray(o);  //  Keep weak tables gray.
      return sizeof(GCtab) + sizeof(TValue) * t->asize + (t->hmask ? sizeof(Node) * (t->hmask + 1) : 0);
   }
   else if (LJ_LIKELY(gct IS ~LJ_TFUNC)) {
      GCfunc* fn = gco_to_function(o);
      gc_traverse_func(g, fn);
      return isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) : sizeCfunc((MSize)fn->c.nupvalues);
   }
   else if (LJ_LIKELY(gct IS ~LJ_TPROTO)) {
      GCproto* pt = gco_to_proto(o);
      gc_traverse_proto(g, pt);
      return pt->sizept;
   }
   else if (LJ_LIKELY(gct IS ~LJ_TTHREAD)) {
      lua_State* th = gco_to_thread(o);
      setgcrefr(th->gclist, g->gc.grayagain);
      setgcref(g->gc.grayagain, o);
      black2gray(o);  //  Threads are never black.
      gc_traverse_thread(g, th);
      return sizeof(lua_State) + sizeof(TValue) * th->stacksize;
   }
   else if (gct IS ~LJ_TARRAY) {
      // Arrays can reach the gray list via IR_TBAR (back barrier emitted by JIT for GC-reference stores).
      GCarray *arr = gco_to_array(o);
      gc_traverse_array(g, arr);
      return sizeof(GCarray) + arr->len * arr->elemsize;
   }
   else {
      GCtrace *T = gco2trace(o);
      gc_traverse_trace(g, T);
      return ((sizeof(GCtrace) + 7) & ~7) + (T->nins - T->nk) * sizeof(IRIns) +
         T->nsnap * sizeof(SnapShot) + T->nsnapmap * sizeof(SnapEntry);
   }
}

//********************************************************************************************************************
// Propagate all gray objects.

static size_t gc_propagate_gray(global_State *g)
{
   size_t m = 0;
   while (gcref(g->gc.gray) != nullptr) m += propagatemark(g);
   return m;
}

//********************************************************************************************************************
// Sweep phase

// Type of GC free functions.

using GCFreeFunc = void (*)(global_State*, GCobj*);

// GC free functions for LJ_TSTR .. LJ_TARRAY. ORDER LJ_T
// Using std::array for type-safe bounds checking and modern C++ semantics.

static const std::array<GCFreeFunc, 10> gc_freefunc = {{
   (GCFreeFunc)lj_str_free,       // LJ_TSTR
   (GCFreeFunc)lj_func_freeuv,    // LJ_TUPVAL
   (GCFreeFunc)lj_state_free,     // LJ_TTHREAD
   (GCFreeFunc)lj_func_freeproto, // LJ_TPROTO
   (GCFreeFunc)lj_func_free,      // LJ_TFUNC
   (GCFreeFunc)lj_trace_free,     // LJ_TTRACE
   (GCFreeFunc)lj_object_free,    // LJ_TOBJECT (native Kotuku object)
   (GCFreeFunc)lj_tab_free,       // LJ_TTAB
   (GCFreeFunc)lj_udata_free,     // LJ_TUDATA
   (GCFreeFunc)lj_array_free      // LJ_TARRAY
}};


// Full sweep of a GC list (sweeps all objects without limit).
// Note: Return value may be discarded when sweeping for side effects only.

inline GCRef* gc_fullsweep(global_State *g, GCRef* p) noexcept { return gc_sweep(g, p, ~uint32_t(0)); }

//********************************************************************************************************************
// Partial sweep of a GC list.

static GCRef* gc_sweep(global_State *g, GCRef* p, uint32_t lim)
{
   // Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown.
   int ow = otherwhite(g);
   GCobj* o;
   while ((o = gcref(*p)) != nullptr and lim-- > 0) {
      if (o->gch.gct IS ~LJ_TTHREAD)  //  Need to sweep open upvalues, too.
         gc_fullsweep(g, &gco_to_thread(o)->openupval);

      if (((o->gch.marked ^ LJ_GC_WHITES) & ow)) {  // Black or current white?
         lj_assertG(not isdead(g, o) or (o->gch.marked & LJ_GC_FIXED), "sweep of undead object");
         makewhite(g, o);  //  Value is alive, change to the current white.
         p = &o->gch.nextgc;
      }
      else {  // Otherwise value is dead, free it.
#ifdef LUA_USE_ASSERT
         if (not (isdead(g, o) or ow IS LJ_GC_SFIXED)) {
            // NB: The indicated type isn't necessarily the source of the problem, it may be a symptom (e.g.
            // managed by the thing that has resource management issues).
            char buffer[80];
            int i = ~o->gch.gct;
            CSTRING name = (i >= 0 and i < 16) ? lj_obj_itypename[i] : "unknown";
            snprintf(buffer, sizeof(buffer), "sweep of unlive object, type: %s", name);
            lj_assertG(false, buffer);
         }
#endif
         setgcrefr(*p, o->gch.nextgc);
         if (o IS gcref(g->gc.root)) setgcrefr(g->gc.root, o->gch.nextgc);  //  Adjust list anchor.
         gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
      }
   }
   return p;
}

//********************************************************************************************************************
// Sweep one string interning table chain. Preserves hashalg bit.

static void gc_sweepstr(global_State *g, GCRef* chain)
{
   // Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown.
   int ow = otherwhite(g);
   uintptr_t u = gcrefu(*chain);
   GCRef q;
   GCRef *p = &q;
   GCobj *o;
   setgcrefp(q, (u & ~(uintptr_t)1));
   while ((o = gcref(*p)) != nullptr) {
      if (((o->gch.marked ^ LJ_GC_WHITES) & ow)) {  // Black or current white?
         lj_assertG(not isdead(g, o) or (o->gch.marked & LJ_GC_FIXED), "sweep of undead string");
         makewhite(g, o);  //  String is alive, change to the current white.
         p = &o->gch.nextgc;
      }
      else {  // Otherwise string is dead, free it.
         lj_assertG(isdead(g, o) or ow IS LJ_GC_SFIXED, "sweep of unlive string");
         setgcrefr(*p, o->gch.nextgc);
         lj_str_free(g, gco_to_string(o));
      }
   }
   setgcrefp(*chain, (gcrefu(q) | (u & 1)));
}

//********************************************************************************************************************
// Check whether we can clear a key or a value slot from a table.

[[nodiscard]] static int gc_mayclear(cTValue* o, int val)
{
   if (tvisgcv(o)) {  // Only collectable objects can be weak references.
      if (tvisstr(o)) {  // But strings cannot be used as weak references.
         gc_mark_str(strV(o));  //  And need to be marked.
         return 0;
      }
      if (iswhite(gcV(o))) return 1;  //  Object is about to be collected.
      if (tvisudata(o) and val and isfinalized(udataV(o))) return 1;  //  Finalized userdata is dropped only from values.
   }
   return 0;  //  Cannot clear.
}

//********************************************************************************************************************
// Clear collected entries from weak tables.

static void gc_clearweak(global_State *g, GCobj* o)
{
   while (o) {
      GCtab* t = gco_to_table(o);
      lj_assertG((t->marked & LJ_GC_WEAK), "clear of non-weak table");

      // Clear array part (TValue has alignment attributes incompatible with std::span).
      if ((t->marked & LJ_GC_WEAKVAL) and t->asize > 0) {
         TValue *array_start = arrayslot(t, 0);
         for (MSize i = 0; i < t->asize; i++) {
            if (gc_mayclear(&array_start[i], 1)) setnilV(&array_start[i]);
         }
      }

      // Clear hash part using std::span.
      if (t->hmask > 0) {
         std::span<Node> hash_part(noderef(t->node), t->hmask + 1);
         for (Node &n : hash_part) {
            if (not tvisnil(&n.val) and (gc_mayclear(&n.key, 0) or gc_mayclear(&n.val, 1)))
               setnilV(&n.val);
         }
      }
      o = gcref(t->gclist);
   }
}

//********************************************************************************************************************
// Call a userdata finaliser.

static void gc_call_finaliser(global_State *g, lua_State *L, cTValue* mo, GCobj* o)
{
   lj_trace_abort(g);

   // Use RAII guard for hook state and GC threshold preservation.
   GCFinaliserGuard guard(g);

   if (LJ_HASPROFILE and (guard.savedHook() & HOOK_PROFILE)) lj_dispatch_update(g);

   // Set up the stack for the finaliser call.

   TValue *top = L->top;
   copyTV(L, top++, mo);
   if (LJ_FR2) setnilV(top++);
   setgcV(L, top, o, ~o->gch.gct);
   L->top = top + 1;

   // Call the finaliser. Stack: |mo|o| -> |
   int errcode = lj_vm_pcall(L, top, 1 + 0, -1);

   if (LJ_HASPROFILE and (guard.savedHook() & HOOK_PROFILE)) lj_dispatch_update(g);

   // Guard destructor restores hook state and threshold here.  Propagate errors after state restoration.

   if (errcode) lj_err_throw(L, errcode);
}

//********************************************************************************************************************
// Finalize one object from the mmudata list (handles both userdata and GCobject).

static void gc_finalize(lua_State *L)
{
   global_State *g = G(L);
   GCobj* o = gcnext(gcref(g->gc.mmudata));
   lj_assertG(tvref(g->jit_base) IS nullptr, "finaliser called on trace");

   // Unchain from list of objects to be finalized.
   if (o IS gcref(g->gc.mmudata)) setgcrefnull(g->gc.mmudata);
   else setgcrefr(gcref(g->gc.mmudata)->gch.nextgc, o->gch.nextgc);

   // Add object back to its original list and make it white.
   if (o->gch.gct IS ~LJ_TOBJECT) {
      // GCobject goes back to the main GC root list.
      setgcrefr(o->gch.nextgc, g->gc.root);
      setgcref(g->gc.root, o);
      makewhite(g, o);
      // Call the finalization function directly (no metamethod lookup).
      lj_object_finalize(L, gco_to_object(o));
   }
   else {
      // Userdata goes back to the main userdata list.
      setgcrefr(o->gch.nextgc, mainthread(g)->nextgc);
      setgcref(mainthread(g)->nextgc, o);
      makewhite(g, o);
      // Resolve the __gc metamethod from userdata's metatable.
      cTValue *mo = lj_meta_fastg(g, tabref(gco_to_userdata(o)->metatable), MM_gc);
      if (mo) gc_call_finaliser(g, L, mo, o);
   }
}

//********************************************************************************************************************
// Finalize all userdata objects from mmudata list.

void lj_gc_finalize_udata(lua_State *L)
{
   while (gcref(G(L)->gc.mmudata) != nullptr) gc_finalize(L);
}

//********************************************************************************************************************
// Free all remaining GC objects.

void lj_gc_freeall(global_State *g)
{
   MSize i, strmask;
   // Free everything, except super-fixed objects (the main thread).
   g->gc.currentwhite = LJ_GC_WHITES | LJ_GC_SFIXED;
   gc_fullsweep(g, &g->gc.root);
   strmask = g->str.mask;
   for (i = 0; i <= strmask; i++)  //  Free all string hash chains.
      gc_sweepstr(g, &g->str.tab[i]);
}

//********************************************************************************************************************
// Atomic part of the GC cycle, transitioning from mark to sweep phase.

static void atomic(global_State *g, lua_State *L)
{
   size_t udsize;

   gc_mark_uv(g);  //  Need to remark open upvalues (the thread may be dead).
   gc_propagate_gray(g);  //  Propagate any left-overs.

   setgcrefr(g->gc.gray, g->gc.weak);  //  Empty the list of weak tables.
   setgcrefnull(g->gc.weak);
   lj_assertG(not iswhite(obj2gco(mainthread(g))), "main thread turned white");
   gc_markobj(g, L);  //  Mark running thread.
   gc_traverse_curtrace(g);  //  Traverse current trace.
   gc_mark_gcroot(g);  //  Mark GC roots (again).
   gc_propagate_gray(g);  //  Propagate all of the above.

   setgcrefr(g->gc.gray, g->gc.grayagain);  //  Empty the 2nd chance list.
   setgcrefnull(g->gc.grayagain);
   gc_propagate_gray(g);  //  Propagate it.

   udsize = lj_gc_separateudata(g, 0);  //  Separate userdata to be finalized.
   udsize += gc_separateobjects(g, 0);  //  Separate GCobjects to be finalized.
   gc_mark_mmudata(g);  //  Mark them.
   udsize += gc_propagate_gray(g);  //  And propagate the marks.

   // All marking done, clear weak tables.
   gc_clearweak(g, gcref(g->gc.weak));

   lj_buf_shrink(L, &g->tmpbuf);  //  Shrink temp buffer.

   // Prepare for sweep phase.
   g->gc.currentwhite = (uint8_t)otherwhite(g);  //  Flip current white.
   g->strempty.marked = g->gc.currentwhite;
   setmref(g->gc.sweep, &g->gc.root);
   g->gc.estimate = g->gc.total - (GCSize)udsize;  //  Initial estimate.
}

//********************************************************************************************************************
// GC state machine. Returns a cost estimate for each step performed.

static size_t gc_onestep(lua_State *L)
{
   global_State *g = G(L);
   switch (GCPhase(g->gc.state)) {
      case GCPhase::Pause:
         gc_mark_start(g);  //  Start a new GC cycle by marking all GC roots.
         return 0;

      case GCPhase::Propagate:
         if (gcref(g->gc.gray) != nullptr) return propagatemark(g);  //  Propagate one gray object.
         g->gc.state = (GCPhase::Atomic);  //  End of mark phase.
         return 0;

      case GCPhase::Atomic:
         if (tvref(g->jit_base)) return LJ_MAX_MEM; //  Don't run atomic phase on trace.
         atomic(g, L);
         g->gc.state = (GCPhase::SweepString);  //  Start of sweep phase.
         g->gc.sweepstr = 0;
         return 0;

      case GCPhase::SweepString: {
         GCSize old = g->gc.total;
         gc_sweepstr(g, &g->str.tab[g->gc.sweepstr++]);  //  Sweep one chain.
         if (g->gc.sweepstr > g->str.mask)
            g->gc.state = (GCPhase::Sweep);  //  All string hash chains sweeped.
         lj_assertG(old >= g->gc.total, "sweep increased memory");
         g->gc.estimate -= old - g->gc.total;
         return GCSWEEPCOST;
      }

      case GCPhase::Sweep: {
         GCSize old = g->gc.total;
         setmref(g->gc.sweep, gc_sweep(g, mref<GCRef>(g->gc.sweep), GCSWEEPMAX));
         lj_assertG(old >= g->gc.total, "sweep increased memory");
         g->gc.estimate -= old - g->gc.total;
         if (gcref(*mref<GCRef>(g->gc.sweep)) IS nullptr) {
            if (g->str.num <= (g->str.mask >> 2) and g->str.mask > LJ_MIN_STRTAB * 2 - 1)
               lj_str_resize(L, g->str.mask >> 1);  //  Shrink string table.
            if (gcref(g->gc.mmudata)) {  // Need any finalizations?
               g->gc.state = GCPhase::Finalize;
            }
            else {  // Otherwise skip this phase to help the JIT.
               g->gc.state = (GCPhase::Pause);  //  End of GC cycle.
               g->gc.debt = 0;
            }
         }
         return GCSWEEPMAX * GCSWEEPCOST;
      }

      case GCPhase::Finalize:
         if (gcref(g->gc.mmudata) != nullptr) {
            GCSize old = g->gc.total;
            if (tvref(g->jit_base)) return LJ_MAX_MEM; //  Don't call finalisers on trace.
            gc_finalize(L);  //  Finalize one userdata object.
            if (old >= g->gc.total and g->gc.estimate > old - g->gc.total) g->gc.estimate -= old - g->gc.total;
            if (g->gc.estimate > GCFINALIZECOST) g->gc.estimate -= GCFINALIZECOST;
            return GCFINALIZECOST;
         }
         g->gc.state = (GCPhase::Pause);  //  End of GC cycle.
         g->gc.debt = 0;
         return 0;
   }
   lj_assertG(0, "bad GC state");
   return 0;
}

// Perform a limited amount of incremental GC steps.
int lj_gc_step(lua_State *L)
{
   global_State *g = G(L);
   VMStateGuard vm_guard(g);  // RAII: saves vmstate, sets to GC, restores on exit.

   GCSize lim = (GCSTEPSIZE / 100) * g->gc.stepmul;
   if (lim IS 0) lim = LJ_MAX_MEM;
   if (g->gc.total > g->gc.threshold) g->gc.debt += g->gc.total - g->gc.threshold;

   do {
      lim -= (GCSize)gc_onestep(L);
      if (g->gc.state IS (GCPhase::Pause)) {
         g->gc.threshold = (g->gc.estimate / 100) * g->gc.pause;
         return 1;  // Finished a GC cycle.
      }
   } while (sizeof(lim) IS 8 ? ((int64_t)lim > 0) : ((int32_t)lim > 0));

   if (g->gc.debt < GCSTEPSIZE) {
      g->gc.threshold = g->gc.total + GCSTEPSIZE;
      return -1;
   }
   else {
      g->gc.debt -= GCSTEPSIZE;
      g->gc.threshold = g->gc.total;
      return 0;
   }
}

//********************************************************************************************************************
// Ditto, but fix the stack top first.

void lj_gc_step_fixtop(lua_State *L)
{
   if (curr_funcisL(L)) L->top = curr_topL(L);
   lj_gc_step(L);
}

//********************************************************************************************************************
// Perform multiple GC steps. Called from JIT-compiled code.

int lj_gc_step_jit(global_State *g, MSize steps)
{
   lua_State *L = gco_to_thread(gcref(g->cur_L));
   L->base = tvref(G(L)->jit_base);
   L->top = curr_topL(L);
   while (steps-- > 0 and lj_gc_step(L) IS 0);
   // Return 1 to force a trace exit.
   return (G(L)->gc.state IS (GCPhase::Atomic) or G(L)->gc.state IS (GCPhase::Finalize));
}

//********************************************************************************************************************
// Perform a full GC cycle.

void lj_gc_fullgc(lua_State *L)
{
   pf::Log(__FUNCTION__).detail("Running full cycle");

   global_State *g = G(L);
   VMStateGuard vm_guard(g);  // RAII: saves vmstate, sets to GC, restores on exit.

   if (g->gc.state <= (GCPhase::Atomic)) {  // Caught somewhere in the middle.
      setmref(g->gc.sweep, &g->gc.root);  // Sweep everything (preserving it).
      setgcrefnull(g->gc.gray);  // Reset lists from partial propagation.
      setgcrefnull(g->gc.grayagain);
      setgcrefnull(g->gc.weak);
      g->gc.state = (GCPhase::SweepString);  // Fast forward to the sweep phase.
      g->gc.sweepstr = 0;
   }

   // Finish any pending sweep.

   while (g->gc.state IS (GCPhase::SweepString) or g->gc.state IS (GCPhase::Sweep)) gc_onestep(L);
   lj_assertG(g->gc.state IS (GCPhase::Finalize) or g->gc.state IS (GCPhase::Pause), "bad GC state");

   // Now perform a full GC.

   g->gc.state = (GCPhase::Pause);
   do { gc_onestep(L); } while (g->gc.state != (GCPhase::Pause));
   g->gc.threshold = (g->gc.estimate / 100) * g->gc.pause;
}

//********************************************************************************************************************
// Move the GC propagation frontier forward.

void lj_gc_barrierf(global_State *g, GCobj* o, GCobj* v)
{
   lj_assertG(isblack(o) and iswhite(v) and !isdead(g, v) and !isdead(g, o), "bad object states for forward barrier");
   lj_assertG(g->gc.state != GCPhase::Finalize and g->gc.state != GCPhase::Pause, "bad GC state");
   lj_assertG(o->gch.gct != ~LJ_TTAB, "barrier object is not a table");
   // Preserve invariant during propagation. Otherwise it doesn't matter.
   if (g->gc.state IS (GCPhase::Propagate) or g->gc.state IS (GCPhase::Atomic)) gc_mark(g, v);  //  Move frontier forward.
   else makewhite(g, o);  //  Make it white to avoid the following barrier.
}

// Specialized barrier for closed upvalue. Pass &uv->tv.

void lj_gc_barrieruv(global_State *g, TValue* tv)
{
#define TV2MARKED(x) \
  (*((uint8_t *)(x) - offsetof(GCupval, tv) + offsetof(GCupval, marked)))
   if (g->gc.state IS (GCPhase::Propagate) or g->gc.state IS (GCPhase::Atomic)) gc_mark(g, gcV(tv));
   else
      TV2MARKED(tv) = (TV2MARKED(tv) & (uint8_t)~LJ_GC_COLORS) | curwhite(g);
#undef TV2MARKED
}

//********************************************************************************************************************
// Close upvalue. Also needs a write barrier.

void lj_gc_closeuv(global_State *g, GCupval* uv)
{
   GCobj* o = obj2gco(uv);
   // Copy stack slot to upvalue itself and point to the copy.
   copyTV(mainthread(g), &uv->tv, uvval(uv));
   setmref(uv->v, &uv->tv);
   uv->closed = 1;
   setgcrefr(o->gch.nextgc, g->gc.root);
   setgcref(g->gc.root, o);
   if (isgray(o)) {  // A closed upvalue is never gray, so fix this.
      if (g->gc.state IS (GCPhase::Propagate) or g->gc.state IS (GCPhase::Atomic)) {
         gray2black(o);  //  Make it black and preserve invariant.
         if (tviswhite(&uv->tv)) lj_gc_barrierf(g, o, gcV(&uv->tv));
      }
      else {
         makewhite(g, o);  //  Make it white, i.e. sweep the upvalue.
         lj_assertG(g->gc.state != (GCPhase::Finalize) and g->gc.state != (GCPhase::Pause),
            "bad GC state");
      }
   }
}

//********************************************************************************************************************
// Mark a trace if it's saved during the propagation phase.

void lj_gc_barriertrace(global_State *g, uint32_t traceno)
{
   if (g->gc.state IS (GCPhase::Propagate) or g->gc.state IS (GCPhase::Atomic))
      gc_marktrace(g, traceno);
}

//********************************************************************************************************************
// Call pluggable memory allocator to allocate or resize a fragment.

void* lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz)
{
   global_State *g = G(L);
   lj_assertG((osz IS 0) IS (p IS nullptr), "realloc API violation");
   p = g->allocf(g->allocd, p, osz, nsz);
   if (p IS nullptr and nsz > 0) lj_err_mem(L);
   lj_assertG((nsz IS 0) IS (p IS nullptr), "allocf API violation");
   lj_assertG(checkptrGC(p), "allocated memory address %p outside required range", p);
   g->gc.total = (g->gc.total - osz) + nsz;
   return p;
}

//********************************************************************************************************************
// Allocate new GC object and link it to the root set.

void * lj_mem_newgco(lua_State *L, GCSize size)
{
   global_State *g = G(L);
   GCobj* o = (GCobj*)g->allocf(g->allocd, nullptr, 0, size);
   if (o IS nullptr) lj_err_mem(L);
   lj_assertG(checkptrGC(o), "allocated memory address %p outside required range", o);
   g->gc.total += size;
   setgcrefr(o->gch.nextgc, g->gc.root);
   setgcref(g->gc.root, o);
   newwhite(g, o);
   return o;
}

//********************************************************************************************************************
// Resize growable vector.

void * lj_mem_grow(lua_State *L, void* p, MSize* szp, MSize lim, MSize esz)
{
   MSize sz = (*szp) << 1;
   if (sz < LJ_MIN_VECSZ) sz = LJ_MIN_VECSZ;
   if (sz > lim) sz = lim;
   p = lj_mem_realloc(L, p, (*szp) * esz, sz * esz);
   *szp = sz;
   return p;
}
