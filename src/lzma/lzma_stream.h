// RAII wrapper for the vendored LZMA SDK decoder (CLzmaDec).  It pairs the decoder state with its allocator and a
// "props consumed" latch so the matching LzmaDec_Free() call can never be forgotten or duplicated, and so the 5-byte
// properties header is parsed exactly once per stream.
//
// The wrapper is pinned in place (move and copy deleted) because it is embedded directly as a member within its owning
// class.  No throwing paths are present; the init helper returns the raw SDK result code (SRes) in keeping with the
// project's error-by-return-value convention.
//
// This header must be included after the LZMA SDK's LzmaDec.h.

#pragma once

struct LZMADecoder {
   CLzmaDec dec;       // The SDK decoder state.  Valid only while Allocated is true.
   ISzAlloc alloc;     // Allocator bridging the decoder onto the Core's memory routines.
   bool allocated;     // True once LzmaDec_Allocate() has succeeded and LzmaDec_Free() is owed.
   bool props_set;     // True once the 5-byte properties header has been consumed for the current stream.

   static void * lzma_alloc(ISzAllocPtr, size_t Size) {
      APTR mem;
      if (AllocMemory(Size, MEM::DATA|MEM::NO_CLEAR, &mem) != ERR::Okay) return nullptr;
      return mem;
   }

   static void lzma_free(ISzAllocPtr, void *Address) {
      if (Address) FreeResource(Address);
   }

   LZMADecoder() {
      LzmaDec_Construct(&dec);
      alloc.Alloc = &lzma_alloc;
      alloc.Free  = &lzma_free;
      allocated   = false;
      props_set   = false;
   }

   ~LZMADecoder() { reset(); }

   LZMADecoder(const LZMADecoder &) = delete;
   LZMADecoder(LZMADecoder &&) = delete;
   LZMADecoder & operator=(const LZMADecoder &) = delete;
   LZMADecoder & operator=(LZMADecoder &&) = delete;

   // Allocate the decoder from a 5-byte properties header and prime it for a new stream.  Any previously allocated
   // state is released first.  Returns the raw SDK result code, which is SZ_OK on success.

   SRes allocate(const Byte *Props) {
      reset();
      auto err = LzmaDec_Allocate(&dec, Props, LZMA_PROPS_SIZE, &alloc);
      if (err IS SZ_OK) {
         LzmaDec_Init(&dec);
         allocated = true;
         props_set = true;
      }
      return err;
   }

   // Release any allocated decoder state.  Idempotent and safe to call regardless of the current state.

   void reset() {
      if (allocated) {
         LzmaDec_Free(&dec, &alloc);
         LzmaDec_Construct(&dec);
         allocated = false;
      }
      props_set = false;
   }

   bool active() const { return allocated; }
};
