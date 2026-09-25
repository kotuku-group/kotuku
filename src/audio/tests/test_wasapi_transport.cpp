#ifdef NDEBUG
#undef NDEBUG
#endif
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <new>
#include <initializer_list>
#include "wasapi.h"

static std::atomic<bool> glTracking{false};
static std::atomic<unsigned> glAllocations{0}, glDeallocations{0};

void *operator new(size_t Size) {
   if (glTracking) ++glAllocations;
   auto memory = std::malloc(Size ? Size : 1);
   if (!memory) std::abort();
   return memory;
}
void *operator new[](size_t Size) { return ::operator new(Size); }
void operator delete(void *Memory) noexcept {
   if (Memory and glTracking) ++glDeallocations;
   std::free(Memory);
}
void operator delete[](void *Memory) noexcept { ::operator delete(Memory); }
void operator delete(void *Memory, size_t) noexcept { ::operator delete(Memory); }
void operator delete[](void *Memory, size_t) noexcept { ::operator delete(Memory); }

#define IS ==
#include "audio_endpoint.h"
struct Source {
   std::atomic<unsigned> Calls{0}, ShortPackets{0};
   std::atomic<bool> Active{true}, Failed{false};
};

static bool produce(void *Context, float *Buffer, unsigned Frames, unsigned Padding)
{
   auto source = (Source *)Context;
   assert(Frames <= 768 and Padding <= 768);
   if (Frames IS 127) ++source->ShortPackets;
   for (unsigned i = 0; i < Frames * 2; ++i) Buffer[i] = source->Active ? 0.25f : 0.0f;
   ++source->Calls;
   return source->Active;
}

static void failed(void *Context, int Error) {
   assert(Error != 0);
   ((Source *)Context)->Failed = true;
}

int main()
{
   AudioEndpointFormat surround{6, 32, 32, 0x3f, true};
   assert(surround.valid());
   float input[] = {0.25f, -0.5f, 2.0f, -2.0f};
   float output[12];
   surround.write(output, input, 2);
   assert(output[0] IS 0.25f and output[1] IS -0.5f and output[6] IS 1.0f and output[7] IS -1.0f);
   for (unsigned i : {2u, 3u, 4u, 5u, 8u, 9u, 10u, 11u}) assert(output[i] IS 0);
   surround.Mask = 0;
   assert(!surround.valid());
   AudioEndpointFormat pcm{2, 24, 24, 3, false};
   uint8_t packed[12];
   pcm.write(packed, input, 2);
   assert(packed[2] IS 0x20 and packed[5] IS 0xc0 and packed[8] IS 0x7f and packed[11] IS 0x80);
   pcm.Bits = 32;
   uint8_t padded[16];
   pcm.write(padded, input, 2);
   for (unsigned i : {0u, 4u, 8u, 12u}) assert(padded[i] IS 0);
   for (unsigned rate : {44100u, 48000u, 96000u}) {
      Source source;
      WasapiFormat format{rate, 2};
      auto stream = wasapi_open("null", format, &source, produce, failed);
      assert(stream and format.Rate IS rate and format.Capacity >= format.Period);
      glTracking = true;
      wasapi_start(stream);
      for (unsigned i = 0; i < 20; ++i) { wasapi_wake(stream); Sleep(2); }
      source.Active = false;
      wasapi_wake(stream);
      Sleep(10);
      source.Active = true;
      wasapi_wake(stream);
      Sleep(10);
      glTracking = false;
      assert(source.Calls > 3 and source.ShortPackets > 0);
      assert(glAllocations IS 0 and glDeallocations IS 0);
      wasapi_test_invalidate(stream);
      for (unsigned i = 0; i < 100 and !source.Failed; ++i) Sleep(1);
      assert(source.Failed);
      wasapi_close(stream);
   }
   // Stop a waiting, never-started stream and reject a bad endpoint without leaking a worker.
   Source source;
   WasapiFormat format{48000, 2};
   auto idle = wasapi_open("null", format, &source, produce, failed);
   assert(idle);
   wasapi_close(idle);
   auto changed = wasapi_open("null", format, &source, produce, failed);
   assert(changed);
   wasapi_test_invalidate(changed);
   Sleep(10);
   assert(!source.Failed); // Failure delivery must wait until the client has published and started the stream.
   wasapi_start(changed);
   for (unsigned i = 0; i < 100 and !source.Failed; ++i) Sleep(1);
   assert(source.Failed);
   wasapi_close(changed);
   assert(!wasapi_open("Kotuku-nonexistent-endpoint", format, &source, produce, failed));
   return 0;
}
