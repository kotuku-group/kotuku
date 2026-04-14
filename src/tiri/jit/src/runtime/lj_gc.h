// Garbage collector.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
//
// Public interface for the LuaJIT garbage collector.
//
// This header provides:
// - GCPhase enum class for type-safe GC state representation
// - GCObjectType C++20 concept for compile-time type constraints
// - Colour test functions (iswhite, isblack, isgray, etc.)
// - Write barrier functions and macros
// - GarbageCollector facade class for modern OOP access
// - Memory allocation functions
//
// Usage:
//   // Using the facade class (recommended for new code)
//   GarbageCollector collector = gc(L);
//   if (collector.isPaused()) {
//      collector.step(L);
//   }
//
//   // Using C-style functions (for compatibility)
//   lj_gc_step(L);
//   lj_gc_fullgc(L);
//
// See also: lj_gc.cpp for implementation details.

#pragma once

#include "lj_obj.h"
#include <concepts>

// Concept for types that can be garbage collected.
// GC objects are accessed via the obj2gco() macro which converts any GC type
// to a GCobj pointer, providing uniform access to the GCHeader fields.

template<typename T>
concept GCObjectType = requires(T* obj) {
   // Must be convertible to GCobj* via obj2gco macro
   { obj2gco(obj) } -> std::convertible_to<GCobj*>;
};

// Bitmasks for marked field of GCobj.
#define LJ_GC_WHITE0    0x01
#define LJ_GC_WHITE1    0x02
#define LJ_GC_BLACK     0x04
#define LJ_GC_FINALIZED 0x08
#define LJ_GC_WEAKKEY   0x08
#define LJ_GC_WEAKVAL   0x10
#define LJ_GC_FIXED     0x20
#define LJ_GC_SFIXED    0x40

#define LJ_GC_WHITES   (LJ_GC_WHITE0 | LJ_GC_WHITE1)
#define LJ_GC_COLORS   (LJ_GC_WHITES | LJ_GC_BLACK)
#define LJ_GC_WEAK   (LJ_GC_WEAKKEY | LJ_GC_WEAKVAL)

// Modern constexpr GC colour test functions (C++20)
[[nodiscard]] constexpr inline bool iswhite(const GCobj* x) noexcept
{
   return (x)->gch.marked & LJ_GC_WHITES;
}

[[nodiscard]] constexpr inline bool isblack(const GCobj* x) noexcept
{
   return (x)->gch.marked & LJ_GC_BLACK;
}

[[nodiscard]] constexpr inline bool isgray(const GCobj* x) noexcept
{
   return !((x)->gch.marked & (LJ_GC_BLACK | LJ_GC_WHITES));
}

[[nodiscard]] inline bool tviswhite(cTValue* x) noexcept
{
   return tvisgcv(x) and iswhite(gcV(x));
}

[[nodiscard]] inline uint8_t otherwhite(const global_State* g) noexcept
{
   return (uint8_t)(g->gc.currentwhite ^ LJ_GC_WHITES);
}

[[nodiscard]] inline bool isdead(const global_State* g, const GCobj* v) noexcept
{
   return (v)->gch.marked & otherwhite(g) & LJ_GC_WHITES;
}

[[nodiscard]] inline uint8_t curwhite(const global_State* g) noexcept
{
   return (uint8_t)((g)->gc.currentwhite & LJ_GC_WHITES);
}

inline void newwhite(global_State* g, void* x) noexcept
{
   ((GCobj*)x)->gch.marked = (uint8_t)curwhite(g);
}

inline void makewhite(global_State* g, GCobj* x) noexcept
{
   x->gch.marked = (uint8_t)((x->gch.marked & (uint8_t)~LJ_GC_COLORS) | curwhite(g));
}

inline void flipwhite(GCobj* x) noexcept
{
   x->gch.marked ^= LJ_GC_WHITES;
}

inline void black2gray(GCobj* x) noexcept
{
   x->gch.marked &= (uint8_t)~LJ_GC_BLACK;
}

inline void fixstring(GCstr* s) noexcept
{
   s->marked |= LJ_GC_FIXED;
}

inline void markfinalized(GCobj* x) noexcept
{
   x->gch.marked |= LJ_GC_FINALIZED;
}

// Collector.
extern "C" size_t lj_gc_separateudata(global_State* g, int all);
extern "C" void lj_gc_finalize_udata(lua_State* L);
extern "C" void lj_gc_freeall(global_State* g);
extern "C" int lj_gc_step(lua_State* L);
extern "C" void lj_gc_step_fixtop(lua_State* L);
extern "C" int lj_gc_step_jit(global_State* g, MSize steps);
extern "C" void lj_gc_fullgc(lua_State* L);

// GC check: drive collector forward if the GC threshold has been reached.
#define lj_gc_check(L) { if (LJ_UNLIKELY(G(L)->gc.total >= G(L)->gc.threshold)) lj_gc_step(L); }
#define lj_gc_check_fixtop(L) { if (LJ_UNLIKELY(G(L)->gc.total >= G(L)->gc.threshold)) lj_gc_step_fixtop(L); }

// Write barriers.
extern "C" void lj_gc_barrierf(global_State* g, GCobj* o, GCobj* v);
extern "C" void lj_gc_barrieruv(global_State* g, TValue* tv);
extern "C" void lj_gc_closeuv(global_State* g, GCupval* uv);
extern "C" void lj_gc_barriertrace(global_State* g, uint32_t traceno);

// Move the GC propagation frontier back for tables (make it gray again).
static LJ_AINLINE void lj_gc_barrierback(global_State* g, GCtab* t)
{
   GCobj *o = obj2gco(t);
   lj_assertG(isblack(o) and !isdead(g, o), "bad object states for backward barrier");
   lj_assertG(g->gc.state != GCPhase::Finalize and g->gc.state != GCPhase::Pause, "bad GC state");
   black2gray(o);
   setgcrefr(t->gclist, g->gc.grayagain);
   setgcref(g->gc.grayagain, o);
}

// Barrier for stores to table objects. TValue and GCobj variant.
#define lj_gc_anybarriert(L, t)  { if (LJ_UNLIKELY(isblack(obj2gco(t)))) lj_gc_barrierback(G(L), (t)); }
#define lj_gc_barriert(L, t, tv) { if (tviswhite(tv) and isblack(obj2gco(t))) lj_gc_barrierback(G(L), (t)); }
#define lj_gc_objbarriert(L, t, o)  { if (iswhite(obj2gco(o)) and isblack(obj2gco(t))) lj_gc_barrierback(G(L), (t)); }

// Barrier for stores to any other object. TValue and GCobj variant.
#define lj_gc_barrier(L, p, tv) { if (tviswhite(tv) and isblack(obj2gco(p))) lj_gc_barrierf(G(L), obj2gco(p), gcV(tv)); }
#define lj_gc_objbarrier(L, p, o) { if (iswhite(obj2gco(o)) and isblack(obj2gco(p))) lj_gc_barrierf(G(L), obj2gco(p), obj2gco(o)); }

// GarbageCollector Facade Class
//
// This is a lightweight facade that delegates to the existing C-style functions.
//
// Method Categories:
// - State Queries: phase(), totalMemory(), isPaused(), isMarking(), etc.
// - Collection Control: step(), fullCycle(), check()
// - Write Barriers: barrierForward(), barrierBack(), barrierUpvalue()
// - Finalization: separateUdata(), finalizeUdata(), freeAll()
// - Upvalue Management: closeUpvalue()
// - JIT Integration: barrierTrace(), stepJit()
//
// Example Usage:
//   // Create facade from lua_State or global_State
//   GarbageCollector collector = gc(L);
//
//   // Query GC state
//   if (collector.isPaused()) {
//      std::cout << "GC is idle\n";
//   }
//
//   // Perform collection
//   collector.step(L);           // Incremental step
//   collector.fullCycle(L);      // Full collection
//
//   // Check memory usage
//   GCSize total = collector.totalMemory();
//   GCSize threshold = collector.threshold();

class GarbageCollector {
   global_State *gs;

public:
   // Construct a GarbageCollector facade for the given global state.
   explicit GarbageCollector(global_State* g) noexcept : gs(g) {}

   // Non-copyable but movable (references global state).
   GarbageCollector(const GarbageCollector&) = default;
   GarbageCollector& operator=(const GarbageCollector&) = default;

   // -- State Queries --

   // Get the current GC phase.
   [[nodiscard]] GCPhase phase() const noexcept {
      return static_cast<GCPhase>(gs->gc.state);
   }

   // Get the current GC phase as raw uint8_t (for compatibility).
   [[nodiscard]] uint8_t phaseRaw() const noexcept {
      return uint8_t(gs->gc.state);
   }

   // Get total memory currently allocated.
   [[nodiscard]] GCSize totalMemory() const noexcept {
      return gs->gc.total;
   }

   // Get the GC threshold (collection triggers when total >= threshold).
   [[nodiscard]] GCSize threshold() const noexcept {
      return gs->gc.threshold;
   }

   // Get the estimated live memory after last collection.
   [[nodiscard]] GCSize estimate() const noexcept {
      return gs->gc.estimate;
   }

   // Get the current GC debt (how far behind schedule the GC is).
   [[nodiscard]] GCSize debt() const noexcept {
      return gs->gc.debt;
   }

   // Check if GC is currently paused.
   [[nodiscard]] bool isPaused() const noexcept {
      return gs->gc.state IS (GCPhase::Pause);
   }

   // Check if GC is in mark phase (propagate or atomic).
   [[nodiscard]] bool isMarking() const noexcept {
      return gs->gc.state IS (GCPhase::Propagate) or gs->gc.state IS (GCPhase::Atomic);
   }

   // Check if GC is in sweep phase.
   [[nodiscard]] bool isSweeping() const noexcept {
      return gs->gc.state IS GCPhase::SweepString or gs->gc.state IS GCPhase::Sweep;
   }

   // Check if GC is in finalize phase.
   [[nodiscard]] bool isFinalizing() const noexcept {
      return gs->gc.state IS GCPhase::Finalize;
   }

   // Check if there are pending finalisers.
   [[nodiscard]] bool hasPendingFinalisers() const noexcept {
      return gcref(gs->gc.mmudata) != nullptr;
   }

   // -- Collection Control --

   // Perform incremental GC steps.
   // Returns: 1 if finished a cycle, -1 if within threshold, 0 otherwise.
   int step(lua_State* L) noexcept {
      return lj_gc_step(L);
   }

   // Perform incremental GC steps, fixing the stack top first.
   void stepFixTop(lua_State* L) noexcept {
      lj_gc_step_fixtop(L);
   }

   // Perform a full GC cycle.
   void fullCycle(lua_State* L) noexcept {
      lj_gc_fullgc(L);
   }

   // Check if GC should run and step if needed.
   void check(lua_State* L) noexcept {
      if (LJ_UNLIKELY(gs->gc.total >= gs->gc.threshold)) {
         lj_gc_step(L);
      }
   }

   // Forward barrier: mark white object when stored in black object.
   void barrierForward(GCobj* parent, GCobj* child) noexcept {
      lj_gc_barrierf(gs, parent, child);
   }

   // Backward barrier: make black table gray when storing to it.
   void barrierBack(GCtab* t) noexcept {
      lj_gc_barrierback(gs, t);
   }

   // Barrier for closed upvalue.
   void barrierUpvalue(TValue* tv) noexcept {
      lj_gc_barrieruv(gs, tv);
   }

   // -- Memory Statistics --

   // Get the pause multiplier (controls delay between cycles).
   [[nodiscard]] MSize pauseMultiplier() const noexcept {
      return gs->gc.pause;
   }

   // Get the step multiplier (controls incremental step size).
   [[nodiscard]] MSize stepMultiplier() const noexcept {
      return gs->gc.stepmul;
   }

   // -- Finalization --

   // Separate userdata with finalisers to the mmudata list.
   // Returns the total size of userdata to be finalized.
   size_t separateUdata(int all) noexcept {
      return lj_gc_separateudata(gs, all);
   }

   // Finalize all pending userdata objects.
   void finalizeUdata(lua_State* L) noexcept {
      lj_gc_finalize_udata(L);
   }

   // Free all GC objects (called during state shutdown).
   void freeAll() noexcept {
      lj_gc_freeall(gs);
   }

   // -- Upvalue Management --

   // Close an upvalue (moves it from stack to heap).
   void closeUpvalue(GCupval* uv) noexcept {
      lj_gc_closeuv(gs, uv);
   }

   // Barrier for trace object during propagation phase.
   void barrierTrace(uint32_t traceno) noexcept {
      lj_gc_barriertrace(gs, traceno);
   }

   // Perform multiple GC steps from JIT-compiled code.
   int stepJit(MSize steps) noexcept {
      return lj_gc_step_jit(gs, steps);
   }

   // -- GC Control Methods --

   // Stop the garbage collector by setting threshold to maximum.
   // This prevents any automatic GC steps until restart() is called.
   void stop() noexcept {
      gs->gc.threshold = LJ_MAX_MEM;
   }

   // Restart the garbage collector after stop().
   // If data == -1, calculates threshold based on pause percentage.
   // Otherwise, sets threshold equal to current memory total.
   void restart(int data = -1) noexcept {
      gs->gc.threshold = (data IS -1)
         ? (gs->gc.total / 100) * gs->gc.pause
         : gs->gc.total;
   }

   // Set GC pause percentage (controls delay between cycles).
   // Returns the previous pause value.
   MSize setPause(MSize pause) noexcept {
      MSize old = gs->gc.pause;
      gs->gc.pause = pause;
      return old;
   }

   // Set GC step multiplier (controls incremental step size).
   // Returns the previous step multiplier value.
   MSize setStepMultiplier(MSize stepmul) noexcept {
      MSize old = gs->gc.stepmul;
      gs->gc.stepmul = stepmul;
      return old;
   }

   // Check if GC is currently running.
   // Returns false if GC was stopped via stop(), true otherwise.
   [[nodiscard]] bool isRunning() const noexcept {
      return gs->gc.threshold != LJ_MAX_MEM;
   }

   // -- Access to underlying state --

   // Get the underlying global_State pointer.
   [[nodiscard]] global_State* globalState() const noexcept {
      return gs;
   }
};

// Factory function to create a GarbageCollector for a lua_State.
[[nodiscard]] inline GarbageCollector gc(lua_State* L) noexcept {
   return GarbageCollector(G(L));
}

// Factory function to create a GarbageCollector for a global_State.
[[nodiscard]] inline GarbageCollector gc(global_State* g) noexcept {
   return GarbageCollector(g);
}

// RAII guards that automatically manage GC state across scopes.
// These are useful for preventing GC during critical operations.

// RAII guard that pauses garbage collection during its lifetime.
// Saves the current threshold on construction and restores it on destruction.
//
// Use Cases:
// - Operations that must complete atomically without GC interruption
// - Performance-critical sections where GC overhead is unacceptable
// - Memory-sensitive code that needs stable pointers
//
// Example Usage:
//   void criticalOperation(global_State* g) {
//      GCPauseGuard pause_gc(g);
//      // GC is paused here - safe to manipulate GC objects
//      // without worrying about collection or reallocation
//      doSomethingCritical();
//   }  // GC threshold restored on scope exit
//
// Thread Safety:
// - Not thread-safe on its own (operates on global_State)
// - Caller must ensure proper synchronization if using from multiple threads
//
// Performance Notes:
// - Lightweight: just two GCSize assignments
// - No heap allocation
// - Minimal overhead for critical sections

class GCPauseGuard {
   global_State *gs;
   GCSize saved_threshold;

public:
   // Construct guard and pause GC by setting threshold to maximum.
   explicit GCPauseGuard(global_State *g) noexcept : gs(g), saved_threshold(g->gc.threshold) {
      g->gc.threshold = LJ_MAX_MEM;  // Prevent GC from triggering
   }

   // Restore the original GC threshold.
   ~GCPauseGuard() noexcept {
      gs->gc.threshold = saved_threshold;
   }

   // Non-copyable, non-movable (manages GC state for specific global_State).
   GCPauseGuard(const GCPauseGuard&) = delete;
   GCPauseGuard& operator=(const GCPauseGuard&) = delete;
   GCPauseGuard(GCPauseGuard&&) = delete;
   GCPauseGuard& operator=(GCPauseGuard&&) = delete;
};

// Allocator.

extern "C" void * lj_mem_realloc(lua_State* L, void* p, GCSize osz, GCSize nsz);
extern "C" void * lj_mem_newgco(lua_State* L, GCSize size);
extern "C" void * lj_mem_grow(lua_State* L, void* p, MSize* szp, MSize lim, MSize esz);

#define lj_mem_new(L, s)   lj_mem_realloc(L, NULL, 0, (s))

static LJ_AINLINE void lj_mem_free(global_State* g, void* p, size_t osize)
{
   g->gc.total -= (GCSize)osize;
   g->allocf(g->allocd, p, osize, 0);
}

#define lj_mem_newvec(L, n, t)   ((t *)lj_mem_new(L, (GCSize)((n)*sizeof(t))))
#define lj_mem_reallocvec(L, p, on, n, t) ((p) = (t *)lj_mem_realloc(L, p, (on)*sizeof(t), (GCSize)((n)*sizeof(t))))
#define lj_mem_growvec(L, p, n, m, t) ((p) = (t *)lj_mem_grow(L, (p), &(n), (m), (MSize)sizeof(t)))
#define lj_mem_freevec(g, p, n, t)   lj_mem_free(g, (p), (n)*sizeof(t))
#define lj_mem_newobj(L, t)   ((t *)lj_mem_newgco(L, sizeof(t)))
#define lj_mem_newt(L, s, t)   ((t *)lj_mem_new(L, (s)))
#define lj_mem_freet(g, p)   lj_mem_free(g, (p), sizeof(*(p)))
