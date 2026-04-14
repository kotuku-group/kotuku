#pragma once

// Simple scoped memory allocation tracker. Usage:
//
// {
//    MemTracker tracker(true);  // Track malloc/free in addition to new/delete
//    ... code to track ...
//    auto stats = tracker.getStats();
// }
//
// NOTE: malloc/free tracking only works if you use the provided wrappers:
//
//   pf::tracked_malloc(), pf::tracked_free(), etc.
//   OR define MEMTRACK_REPLACE_MALLOC before including this header

#include <cstddef>
#include <atomic>
#include <new>
#include <cstdlib>
#include <cstdint>

namespace pf {

class MemTracker {
public:
   struct Stats {
      size_t total_alloc;  // Total number of allocations made
      size_t total_free;   // Total number of frees (might not necessarily match total_alloc)
      size_t total_size;   // Total bytes allocated; not impacted by deallocations
      size_t avg_size() { return total_alloc > 0 ? total_size / total_alloc : 0; }
   };

private:
   // Use atomics in case the tracked code is multi-threaded
   std::atomic<size_t> mTotalAlloc{0};
   std::atomic<size_t> mTotalFree{0};
   std::atomic<size_t> mTotalSize{0};
   bool mTrackMalloc;
   MemTracker *mPrevTracker{nullptr};
   bool mPrevTrackingMalloc{false};

   inline static thread_local MemTracker *glActiveTracker = nullptr;
   inline static thread_local bool glTrackingMalloc = false;

   // Internal allocation hook
   void recordAlloc(size_t Size) {
      mTotalAlloc.fetch_add(1, std::memory_order_relaxed);
      mTotalSize.fetch_add(Size, std::memory_order_relaxed);
   }

   // Internal deallocation hook
   void recordFree() {
      mTotalFree.fetch_add(1, std::memory_order_relaxed);
   }

public:
   MemTracker(bool TrackMalloc = false)
      : mTrackMalloc(TrackMalloc),
        mPrevTracker(glActiveTracker),
        mPrevTrackingMalloc(glTrackingMalloc) {
      glActiveTracker = this;
      glTrackingMalloc = TrackMalloc;
   }

   ~MemTracker() {
      glActiveTracker = mPrevTracker;
      glTrackingMalloc = mPrevTrackingMalloc;
   }

   // Prevent copying and moving
   MemTracker(const MemTracker&) = delete;
   MemTracker& operator=(const MemTracker&) = delete;
   MemTracker(MemTracker&&) = delete;
   MemTracker& operator=(MemTracker&&) = delete;

   Stats getStats() const {
      auto alloc_count = mTotalAlloc.load(std::memory_order_relaxed);
      auto free_count  = mTotalFree.load(std::memory_order_relaxed);
      auto total_bytes = mTotalSize.load(std::memory_order_relaxed);

      return Stats{
         alloc_count,
         free_count,
         total_bytes
      };
   }

   void reset() {
      mTotalAlloc.store(0, std::memory_order_relaxed);
      mTotalFree.store(0, std::memory_order_relaxed);
      mTotalSize.store(0, std::memory_order_relaxed);
   }

   // Allow friend functions to access private members
   friend void* ::operator new(size_t);
   friend void* ::operator new[](size_t);
   friend void ::operator delete(void*) noexcept;
   friend void ::operator delete[](void*) noexcept;
   friend void* tracked_malloc(size_t);
   friend void* tracked_calloc(size_t, size_t);
   friend void* tracked_realloc(void*, size_t);
   friend void tracked_free(void*);
};

// Tracked malloc wrappers (use these when tracking malloc is enabled)

inline void* tracked_malloc(size_t Size) {
   void* ptr = std::malloc(Size);
   if (MemTracker::glTrackingMalloc and MemTracker::glActiveTracker and ptr) {
      MemTracker::glActiveTracker->recordAlloc(Size);
   }
   return ptr;
}

inline void* tracked_calloc(size_t Num, size_t Size) {
   void* ptr = std::calloc(Num, Size);
   if (MemTracker::glTrackingMalloc and MemTracker::glActiveTracker and ptr) {
      MemTracker::glActiveTracker->recordAlloc(Num * Size);
   }
   return ptr;
}

inline void* tracked_realloc(void* Ptr, size_t Size) {
   if (MemTracker::glTrackingMalloc and MemTracker::glActiveTracker) {
      void* new_ptr = std::realloc(Ptr, Size);
      bool freed_block = false;
      if (Ptr) {
         if (not Size) freed_block = true;
         else if (new_ptr) {
            std::uintptr_t new_value = std::uintptr_t(new_ptr);
            std::uintptr_t old_value = std::uintptr_t(Ptr);
            if (new_value - old_value) freed_block = true;
         }
      }
      if (freed_block) MemTracker::glActiveTracker->recordFree();
      if (new_ptr and Size > 0) MemTracker::glActiveTracker->recordAlloc(Size);
      return new_ptr;
   }
   return std::realloc(Ptr, Size);
}

inline void tracked_free(void* Ptr) {
   if (MemTracker::glTrackingMalloc and MemTracker::glActiveTracker and Ptr) {
      MemTracker::glActiveTracker->recordFree();
   }
   std::free(Ptr);
}

} // namespace pf

// Global operator overrides for tracking new/delete

inline void * operator new(size_t Size) {
   void * ptr = std::malloc(Size);
   while (not ptr) {
      std::new_handler handler = std::get_new_handler();
      if (not handler) std::abort();
      handler();
      ptr = std::malloc(Size);
   }

   if (pf::MemTracker::glActiveTracker) pf::MemTracker::glActiveTracker->recordAlloc(Size);
   return ptr;
}

inline void * operator new[](size_t Size) {
   void * ptr = std::malloc(Size);
   while (not ptr) {
      std::new_handler handler = std::get_new_handler();
      if (not handler) std::abort();
      handler();
      ptr = std::malloc(Size);
   }

   if (pf::MemTracker::glActiveTracker) pf::MemTracker::glActiveTracker->recordAlloc(Size);

   return ptr;
}

inline void operator delete(void *Ptr) noexcept {
   if (not Ptr) return;
   if (pf::MemTracker::glActiveTracker) pf::MemTracker::glActiveTracker->recordFree();
   std::free(Ptr);
}

inline void operator delete[](void *Ptr) noexcept {
   if (not Ptr) return;
   if (pf::MemTracker::glActiveTracker) pf::MemTracker::glActiveTracker->recordFree();
   std::free(Ptr);
}

// Optional: Define macros to replace malloc/free globally
#ifdef MEMTRACK_REPLACE_MALLOC
   #define malloc(size) pf::tracked_malloc(size)
   #define calloc(num, size) pf::tracked_calloc(num, size)
   #define realloc(ptr, size) pf::tracked_realloc(ptr, size)
   #define free(ptr) pf::tracked_free(ptr)
#endif
