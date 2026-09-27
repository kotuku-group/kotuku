// Included by audio.cpp to exercise the module implementation.

namespace audio_tests_mixers {

static void unsigned_mono(AudioTestContext &Test)
{
   uint8_t source[] = { 0, 128, 255 };
   float output[3] = {};
   auto dest = output;
   mix_template<uint8_t, false, false, false>(source, 0, 3, 1, 1, 1, &dest);
   AUDIO_CHECK(output[0] IS -1.0f and output[1] IS 0 and output[2] IS 127.0f / 128.0f);
}

static void mono_to_stereo(AudioTestContext &Test)
{
   int16_t source[] = { -1000, 0, 1000 };
   float output[6] = {};
   auto dest = output;
   mix_template<int16_t, false, true, false>(source, 0, 3, 1, 0.5f, 0.8f, &dest);
   for (int i = 0; i < 3; ++i) {
      AUDIO_CHECK(output[i * 2] IS source[i] * (0.5f / 32768.0f));
      AUDIO_CHECK(output[i * 2 + 1] IS source[i] * (0.8f / 32768.0f));
   }
}

static void unsigned_stereo(AudioTestContext &Test)
{
   uint8_t source[] = { 0, 255, 128, 0 };
   float output[4] = {};
   auto dest = output;
   mix_template<uint8_t, true, true, false>(source, 0, 2, 1, 1, 1, &dest);
   AUDIO_CHECK(output[0] IS -1.0f and output[1] IS 127.0f / 128.0f);
   AUDIO_CHECK(output[2] IS 0 and output[3] IS -1.0f);
}

static void volume_accumulation_and_position(AudioTestContext &Test)
{
   // One pass per gain covers scaling, additive mixing and cursor advancement on the same mixer path.
   int16_t source[] = { -1000, 0, 1000 };
   for (float volume : { 0.0f, 0.25f, 0.5f, 1.0f, 2.0f }) {
      float output[] = { 100, 100, 100 };
      auto dest = output;
      const int position = mix_template<int16_t, false, false, false>(source, 0, 3, 1, volume, volume, &dest);
      for (int i = 0; i < 3; ++i) AUDIO_CHECK(output[i] IS 100 + volume * source[i] / 32768.0f);
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
   AUDIO_CHECK(output[0] IS 16383.5f / 32768.0f and output[1] IS 32767.0f / 32768.0f);
   AUDIO_CHECK(output[2] IS 16383.5f / 32768.0f and output[3] IS 0);
   AUDIO_CHECK(position IS 5 * 32768 and dest IS output + 4);
   set_mix_step(65536);
}

static void pcm_endpoints_and_output_conversion(AudioTestContext &Test)
{
   uint8_t source8[] = { 0, 128, 255 };
   const float expected8[] = { -1.0f, 0, 127.0f / 128.0f };
   float mixed8[std::size(source8)]{};
   auto dest8 = mixed8;
   mix_template<uint8_t, false, false, false>(source8, 0, std::size(source8), 1, 1, 1, &dest8);
   AUDIO_CHECK(std::equal(std::begin(mixed8), std::end(mixed8), std::begin(expected8)));
   uint8_t output8[std::size(source8)]{};
   convert_samples<uint8_t>(mixed8, std::size(mixed8), output8);
   AUDIO_CHECK(std::equal(std::begin(output8), std::end(output8), std::begin(source8)));

   int16_t source16[] = { -32768, -32767, -1, 0, 1, 32767 };
   float mixed16[std::size(source16)]{};
   auto dest16 = mixed16;
   mix_template<int16_t, false, false, false>(source16, 0, std::size(source16), 1, 1, 1, &dest16);
   for (size_t i = 0; i < std::size(source16); ++i) {
      AUDIO_CHECK(mixed16[i] IS float(source16[i]) / 32768.0f);
   }
   int16_t output16[std::size(source16)]{};
   convert_samples<int16_t>(mixed16, std::size(mixed16), output16);
   AUDIO_CHECK(std::equal(std::begin(output16), std::end(output16), std::begin(source16)));

   float clipping[] = { -2, -1, -0.5f, 0, 0.5f, 1, 2 };
   const uint8_t clipped8[] = { 0, 0, 64, 128, 192, 255, 255 };
   const int16_t clipped16[] = { -32768, -32768, -16384, 0, 16384, 32767, 32767 };
   uint8_t converted8[std::size(clipping)]{};
   int16_t converted16[std::size(clipping)]{};
   float converted_float[std::size(clipping)]{};
   convert_samples<uint8_t>(clipping, std::size(clipping), converted8);
   convert_samples<int16_t>(clipping, std::size(clipping), converted16);
   convert_samples<float>(clipping, std::size(clipping), converted_float);
   AUDIO_CHECK(std::equal(std::begin(converted8), std::end(converted8), std::begin(clipped8)));
   AUDIO_CHECK(std::equal(std::begin(converted16), std::end(converted16), std::begin(clipped16)));
   const float clipped_float[] = { -1, -1, -0.5f, 0, 0.5f, 1, 1 };
   AUDIO_CHECK(std::equal(std::begin(converted_float), std::end(converted_float), std::begin(clipped_float)));
}

#ifdef __AVX2__
template<typename SampleType>
static void vectorized_parity(AudioTestContext &Test, SampleType *Source)
{
   float scalar[16] = {}, vectorized[16] = {};
   auto scalar_dest = scalar;
   auto vector_dest = vectorized;
   mix_template<SampleType, false, true, false>(Source, 0, 8, 1, 0.25f, 0.75f, &scalar_dest);
   mix_vectorized_mono_to_stereo<SampleType>(Source, 0, 8, 0.25f, 0.75f, &vector_dest);
   AUDIO_CHECK(std::equal(std::begin(scalar), std::end(scalar), std::begin(vectorized)));
}

static void simd_parity(AudioTestContext &Test)
{
   uint8_t source8[] = { 0, 1, 64, 127, 128, 129, 254, 255 };
   int16_t source16[] = { -32768, -32767, -12345, -1, 0, 1, 12345, 32767 };
   vectorized_parity(Test, source8);
   vectorized_parity(Test, source16);
}
#endif

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
   pcm_endpoints_and_output_conversion(Test);
#ifdef __AVX2__
   simd_parity(Test);
#endif
   channel_gain_contract(Test);
}

} // namespace audio_tests_mixers
