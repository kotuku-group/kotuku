#pragma once

#include <cstdint>

// No SDK types cross this boundary.  All COM interfaces belong to the transport thread.

struct WasapiStream;

struct WasapiFormat {
   unsigned Rate = 0;
   unsigned Channels = 0;
   unsigned Period = 0;
   unsigned Capacity = 0;
   char Endpoint[256] = {};
};

struct WasapiDiagnostics {
   uint64_t Wakeups = 0;
   uint64_t EmptyQueues = 0;
   uint64_t Packets = 0;
   uint64_t QueuedTotal = 0;
   uint64_t RenderMicroseconds = 0;
   uint64_t MaximumMicroseconds = 0;
   uint64_t Histogram[64] = {}; // 100 microsecond buckets, with overflow in the last bucket.
   uint64_t ProcessMicroseconds = 0;
   uint64_t ElapsedMicroseconds = 0;
};

using WasapiRender = bool (*)(void *, float *, unsigned, unsigned);
using WasapiFailure = void (*)(void *, int);
WasapiStream *wasapi_open(const char *, WasapiFormat &, void *, WasapiRender, WasapiFailure);
void wasapi_start(WasapiStream *);
void wasapi_close(WasapiStream *, WasapiDiagnostics * = nullptr);
void wasapi_wake(WasapiStream *);
void wasapi_notify(WasapiStream *);
void *wasapi_notification(WasapiStream *);
bool wasapi_beep(int, int);

#ifdef AUDIO_TEST_BACKEND
void wasapi_test_invalidate(WasapiStream *);
#endif
