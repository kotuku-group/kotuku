// Test assertions must execute in Release builds too, including expressions that update buffer state.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <kotuku/main.h>
#include "audio_buffer.h"
#include <cassert>
#include <cmath>
#include <array>

int main()
{
   assert(audio_period_count(-1) IS 2 and audio_period_count(100) IS 16);
   assert(audio_period_frames(-1) IS 32 and audio_period_frames(100000) IS 16384);
   assert(audio_period_count(3) IS 3 and audio_period_frames(256) IS 256);
   for (int bits : {8, 16, 32}) {
      for (bool stereo : {false, true}) {
         const auto bytes = audio_frame_bytes(bits, stereo);
         assert(bytes IS unsigned((bits / 8) * (stereo ? 2 : 1)));
         assert(legacy_period_frames(256 * bytes, bits, stereo) IS 256);
      }
   }
   assert(audio_frame_bytes(24, true) IS 0);
   assert(audio_frame_bytes(0, false) IS 0);
   assert(!audio_buffer_valid(65537, 262148, 4));
   assert(!audio_buffer_valid(256, 1048577, 4));
   assert(!audio_buffer_valid(256, 768, 9));
   // Effective buffer size need not equal requested period count times size.
   assert(audio_buffer_valid(240, 1000, 8));
   assert(audio_latency(1000, 48000) > audio_latency(3 * 256, 48000));
   assert(std::abs(audio_latency(768, 44100) - 0.017414965986) < 1e-10);
   assert(audio_latency(768, 48000) IS 0.016);
   assert(legacy_period_frames(2048, 16, true) IS 512);
   assert(legacy_period_frames(2048, 8, false) IS 2048);
   assert(legacy_period_frames(2048, 32, true) IS 256);
   assert(legacy_period_frames(-1, 16, true) IS 32);
   assert(audio_buffer_valid(256, 768, 4));
   assert(!audio_buffer_valid(0, 768, 4));
   assert(!audio_buffer_valid(256, 511, 4));
   assert(!audio_buffer_valid(256, 768, 0));

   // A short write, EAGAIN and recovery must preserve the unwritten suffix, not produce another period.
   RetainedAudioPeriod period;
   period.produce(256);
   assert(period.accept(37));
   assert(period.Offset IS 37 and period.Remaining IS 219);
   assert(period.accept(0));
   assert(!period.accept(220));
   assert(period.Offset IS 37 and period.Remaining IS 219);
   assert(period.accept(219));
   assert(period.Remaining IS 0 and period.Offset IS 256);
   period.produce(256);
   assert(period.Offset IS 0 and period.Remaining IS 256);

   // One second of stereo PCM tolerates a producer delay shorter than its prefetched duration.
   AudioRingCursor ring;
   constexpr size_t capacity = 48000 * 4;
   assert(ring.publish(capacity, capacity, 4));
   for (int i = 0; i < 150; ++i) assert(ring.consume(256 * 4, capacity, 4));
   assert(ring.Used IS (48000 - 150 * 256) * 4);
   assert(ring.publish(150 * 256 * 4, capacity, 4));
   assert(ring.Used IS capacity);
   assert(ring.consume(capacity, capacity, 4));
   const auto preserved = ring.Read;
   assert(!ring.consume(4, capacity, 4));
   assert(ring.Read IS preserved and ring.Used IS 0);
   assert(!ring.publish(3, capacity, 4));
   assert(!ring.publish(capacity + 4, capacity, 4));
   assert(ring.publish(8, capacity, 4));
   assert(ring.consume(4, capacity, 4));
   assert(ring.Used IS 4);
}
