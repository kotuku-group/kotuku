// Included by audio.cpp to exercise the module implementation.

namespace audio_tests_audio_buffer {

static void run(AudioTestContext &Test)
{
   // These helpers validate negotiated driver values that are not exposed as settable Tiri fields.
   for (int bits : {8, 16, 32}) {
      for (bool stereo : {false, true}) {
         const auto bytes = audio_frame_bytes(bits, stereo);
         AUDIO_CHECK(bytes IS unsigned((bits / 8) * (stereo ? 2 : 1)));
         AUDIO_CHECK(legacy_period_frames(256 * bytes, bits, stereo) IS 256);
      }
   }
   AUDIO_CHECK(audio_frame_bytes(24, true) IS 0);
   AUDIO_CHECK(audio_frame_bytes(0, false) IS 0);
   AUDIO_CHECK(!audio_buffer_valid(65537, 262148, 4));
   AUDIO_CHECK(!audio_buffer_valid(256, 1048577, 4));
   AUDIO_CHECK(!audio_buffer_valid(256, 768, 9));
   // Effective buffer size need not equal requested period count times size.
   AUDIO_CHECK(audio_buffer_valid(240, 1000, 8));
   AUDIO_CHECK(audio_latency(1000, 48000) > audio_latency(3 * 256, 48000));
   AUDIO_CHECK(std::abs(audio_latency(768, 44100) - 0.017414965986) < 1e-10);
   AUDIO_CHECK(audio_latency(768, 48000) IS 0.016);
   AUDIO_CHECK(legacy_period_frames(-1, 16, true) IS 32);
   AUDIO_CHECK(audio_buffer_valid(256, 768, 4));
   AUDIO_CHECK(!audio_buffer_valid(0, 768, 4));
   AUDIO_CHECK(!audio_buffer_valid(256, 511, 4));
   AUDIO_CHECK(!audio_buffer_valid(256, 768, 0));

   // A short write, EAGAIN and recovery must preserve the unwritten suffix, not produce another period.
   RetainedAudioPeriod period;
   period.produce(256);
   AUDIO_CHECK(period.accept(37));
   AUDIO_CHECK(period.Offset IS 37 and period.Remaining IS 219);
   AUDIO_CHECK(period.accept(0));
   AUDIO_CHECK(!period.accept(220));
   AUDIO_CHECK(period.Offset IS 37 and period.Remaining IS 219);
   AUDIO_CHECK(period.accept(219));
   AUDIO_CHECK(period.Remaining IS 0 and period.Offset IS 256);
   period.produce(256);
   AUDIO_CHECK(period.Offset IS 0 and period.Remaining IS 256);

   // One second of stereo PCM tolerates a producer delay shorter than its prefetched duration.
   AudioRingCursor ring;
   constexpr size_t capacity = 48000 * 4;
   AUDIO_CHECK(ring.publish(capacity, capacity, 4));
   for (int i = 0; i < 150; ++i) AUDIO_CHECK(ring.consume(256 * 4, capacity, 4));
   AUDIO_CHECK(ring.Used IS (48000 - 150 * 256) * 4);
   AUDIO_CHECK(ring.publish(150 * 256 * 4, capacity, 4));
   AUDIO_CHECK(ring.Used IS capacity);
   AUDIO_CHECK(ring.consume(capacity, capacity, 4));
   const auto preserved = ring.Read;
   AUDIO_CHECK(!ring.consume(4, capacity, 4));
   AUDIO_CHECK(ring.Read IS preserved and ring.Used IS 0);
   AUDIO_CHECK(!ring.publish(3, capacity, 4));
   AUDIO_CHECK(!ring.publish(capacity + 4, capacity, 4));
   AUDIO_CHECK(ring.publish(8, capacity, 4));
   AUDIO_CHECK(ring.consume(4, capacity, 4));
   AUDIO_CHECK(ring.Used IS 4);
}

} // namespace audio_tests_audio_buffer
