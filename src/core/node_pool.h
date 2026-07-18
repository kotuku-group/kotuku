#pragma once

// Node-recycling pool allocator for the pointer-stable resource maps (glResources / glObjects).
//
// These maps experience balanced, high-volume insert/erase churn (see examples/resource_map_churn.tiri) where the
// dominant cost is the per-node malloc()/free() performed by std::unordered_map.  NodePool recycles fixed-size node
// blocks through a free list so that steady-state churn becomes near allocation-free, whilst preserving the map's
// reference/pointer stability contract: a recycled block is never relocated, only reused at the same address.
//
// THREAD SAFETY: NodePool performs NO internal locking.  Every map that uses it is already serialised behind its own
// mutex (glmResources / glmObjects), so all allocate()/deallocate() calls are mutually exclusive.  Do not
// share a NodePool across maps that are guarded by different mutexes.  Code that locks both registries must acquire
// glmResources before glmObjects.

#include <cstdlib>
#include <cstddef>
#include <memory>

//********************************************************************************************************************
// A single-size free list.  std::unordered_map allocates exactly one node type (always the same size), so a
// single-size pool is a precise fit.  Allocations of any other size (e.g. the bucket array) bypass the free list and
// go straight to the system allocator.

class NodePool {
   private:
      struct FreeNode { FreeNode *Next; };

      FreeNode *Head    = nullptr; // Singly-linked free list of recycled blocks
      size_t   BlockSize = 0;      // The one node size this pool recycles (0 until first observed)
      size_t   Reserved  = 0;      // Count of blocks currently held on the free list
      size_t   PeakLive  = 0;      // High-water mark of simultaneously live blocks, for diagnostics
      size_t   Live      = 0;      // Blocks currently handed out to the map

   public:
      NodePool() = default;
      NodePool(const NodePool &) = delete;
      NodePool & operator=(const NodePool &) = delete;

      ~NodePool() { release(); }

      // Allocate a block of Bytes.  Recycled only when Bytes matches the established node size.

      [[nodiscard]] void * allocate(size_t Bytes) {
         if (not BlockSize) BlockSize = (Bytes >= sizeof(FreeNode)) ? Bytes : sizeof(FreeNode);

         if ((Bytes IS BlockSize) and Head) {
            auto block = Head;
            Head = Head->Next;
            Reserved--;
            Live++;
            if (Live > PeakLive) PeakLive = Live;
            return block;
         }

         auto block = std::malloc((Bytes >= sizeof(FreeNode)) ? Bytes : sizeof(FreeNode));
         if (not block) std::abort();
         if (Bytes IS BlockSize) {
            Live++;
            if (Live > PeakLive) PeakLive = Live;
         }
         return block;
      }

      // Return a block.  Node-sized blocks are recycled onto the free list; all others are released to the system.

      void deallocate(void *Block, size_t Bytes) noexcept {
         if (not Block) return;
         if (Bytes IS BlockSize) {
            auto node = (FreeNode *)Block;
            node->Next = Head;
            Head = node;
            Reserved++;
            if (Live) Live--;
         }
         else std::free(Block);
      }

      // Manually release every recycled block back to the system allocator.  This is the explicit shrink operation;
      // it is never triggered automatically by erase().  Live (handed-out) blocks are unaffected.

      void release() noexcept {
         while (Head) {
            auto next = Head->Next;
            std::free(Head);
            Head = next;
         }
         Reserved = 0;
      }

      size_t reserved() const noexcept { return Reserved; }
      size_t live()     const noexcept { return Live; }
      size_t peakLive() const noexcept { return PeakLive; }
      size_t blockSize() const noexcept { return BlockSize; }
};

//********************************************************************************************************************
// STL-compatible allocator adapter.  std::unordered_map rebinds this to its internal node type; the rebound instances
// all share the same underlying NodePool (held by reference), so node allocations route through the free list while
// the rebound bucket-array allocations (a different size) pass straight through.
//
// Allocator state (the pool reference) is copied on rebind/copy and propagated on container move/swap so that the
// container always allocates and frees against the same pool.

template <class T> class PoolAllocator {
   public:
      using value_type = T;
      using propagate_on_container_copy_assignment = std::true_type;
      using propagate_on_container_move_assignment = std::true_type;
      using propagate_on_container_swap            = std::true_type;
      using is_always_equal                        = std::false_type;

      NodePool *Pool = nullptr;

      PoolAllocator(NodePool &aPool) noexcept : Pool(&aPool) { }

      template <class U> PoolAllocator(const PoolAllocator<U> &Other) noexcept : Pool(Other.Pool) { }

      [[nodiscard]] T * allocate(size_t Count) {
         return (T *)Pool->allocate(Count * sizeof(T));
      }

      void deallocate(T *Ptr, size_t Count) noexcept {
         Pool->deallocate(Ptr, Count * sizeof(T));
      }

      template <class U> bool operator==(const PoolAllocator<U> &Other) const noexcept { return Pool IS Other.Pool; }
      template <class U> bool operator!=(const PoolAllocator<U> &Other) const noexcept { return Pool != Other.Pool; }

      template <class U> struct rebind { using other = PoolAllocator<U>; };
};
