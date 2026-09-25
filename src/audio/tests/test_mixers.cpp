// Included by audio.cpp to exercise the module implementation.

namespace audio_tests_mixers {

static void unsigned_mono(AudioTestContext &Test)
{
   uint8_t source[] = { 0, 128, 255 };
   float output[3] = {};
   auto dest = output;
   mix_template<uint8_t, false, false, false>(source, 0, 3, 1, 1, 1, &dest);
   AUDIO_CHECK(output[0] IS -32768 and output[1] IS 0 and output[2] IS 32512);
}

static void mono_to_stereo(AudioTestContext &Test)
{
   int16_t source[] = { -1000, 0, 1000 };
   float output[6] = {};
   auto dest = output;
   mix_template<int16_t, false, true, false>(source, 0, 3, 1, 0.5f, 0.8f, &dest);
   for (int i = 0; i < 3; ++i) {
      AUDIO_CHECK(output[i * 2] IS source[i] * 0.5f);
      AUDIO_CHECK(output[i * 2 + 1] IS source[i] * 0.8f);
   }
}

static void unsigned_stereo(AudioTestContext &Test)
{
   uint8_t source[] = { 0, 255, 128, 0 };
   float output[4] = {};
   auto dest = output;
   mix_template<uint8_t, true, true, false>(source, 0, 2, 1, 1, 1, &dest);
   AUDIO_CHECK(output[0] IS -32768 and output[1] IS 32512);
   AUDIO_CHECK(output[2] IS 0 and output[3] IS -32768);
}

static void volume_accumulation_and_position(AudioTestContext &Test)
{
   // One pass per gain covers scaling, additive mixing and cursor advancement on the same mixer path.
   int16_t source[] = { -1000, 0, 1000 };
   for (float volume : { 0.0f, 0.25f, 0.5f, 1.0f, 2.0f }) {
      float output[] = { 100, 100, 100 };
      auto dest = output;
      const int position = mix_template<int16_t, false, false, false>(source, 0, 3, 1, volume, volume, &dest);
      for (int i = 0; i < 3; ++i) AUDIO_CHECK(output[i] IS 100 + volume * source[i]);
      AUDIO_CHECK(position IS 3 * 65536 and dest IS output + 3);
   }
}

static void interpolation(AudioTestContext &Test)
{
   int16_t source[] = { 0, 32767, 0, -32767 };
   float output[4] = {};
   auto dest = output;
   set_mix_step(32768);
   const int position = mix_template<int16_t, false, false, true>(source, 32768, 4, 1, 1, 1, &dest);
   AUDIO_CHECK(output[0] IS 16383.5f and output[1] IS 32767);
   AUDIO_CHECK(output[2] IS 16383.5f and output[3] IS 0);
   AUDIO_CHECK(position IS 5 * 32768 and dest IS output + 4);
   set_mix_step(65536);
}

static void channel_gain_contract(AudioTestContext &Test)
{
   // The documented linear balance law, including clamped finite boundaries and mono output ignoring pan.
   struct Case { double Volume, Pan; bool Stereo; double Left, Right; };
   const Case cases[] = {
      { 1.0, 0, true, 1.0, 1.0 }, { 0.5, 0, true, 0.5, 0.5 }, { 0.8, -0.25, true, 0.8, 0.6 },
      { 0.8, 0.25, true, 0.6, 0.8 }, { 1.0, -1.0, true, 1.0, 0 }, { 1.0, 1.0, true, 0, 1.0 },
      { 2.0, -3.0, true, 1.0, 0 }, { -0.5, 0.5, true, 0, 0 }, { 0.5, 1.0, false, 0.5, 0.5 },
      { 1.0, -0.0, true, 1.0, 1.0 }, { 1e308, 1e308, true, 0, 1.0 }
   };
   for (const auto &c : cases) {
      auto gains = channel_gains(c.Volume, c.Pan, c.Stereo);
      AUDIO_CHECK(std::abs(gains.Left - c.Left) < 1e-12 and std::abs(gains.Right - c.Right) < 1e-12);
   }
}

static void run(AudioTestContext &Test)
{
   struct RestoreStep {
      int Value = MixStep;
      ~RestoreStep() { set_mix_step(Value); }
   } restore;
   set_mix_step(65536);
   unsigned_mono(Test);
   mono_to_stereo(Test);
   unsigned_stereo(Test);
   volume_accumulation_and_position(Test);
   interpolation(Test);
   channel_gain_contract(Test);
}

} // namespace audio_tests_mixers
