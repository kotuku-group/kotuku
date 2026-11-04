// Device-free final-converter and public policy tests, included by audio.cpp.
namespace audio_tests_audio_dither {

static void statistics(AudioTestContext &Test)
{
   for (uint64_t seed : { 0ULL, 1ULL, 0x123456789abcdefULL }) {
      AudioDitherState state;
      state.reset(seed);
      double sum[2]{}, squares[2]{}, cross = 0;
      bool bounded = true;
      constexpr int count = 200000;
      for (int i = 0; i < count; ++i) {
         const double left = state.next(2), right = state.next(2);
         bounded = bounded and std::abs(left) < 1 and std::abs(right) < 1;
         sum[0] += left;
         sum[1] += right;
         squares[0] += left * left;
         squares[1] += right * right;
         cross += left * right;
      }
      AUDIO_CHECK(bounded);
      for (int channel = 0; channel < 2; ++channel) {
         AUDIO_CHECK(std::abs(sum[channel] / count) < 0.004);
         AUDIO_CHECK(std::abs(squares[channel] / count - 1.0 / 6) < 0.003);
      }
      AUDIO_CHECK(std::abs(cross / count) < 0.003);
   }
}

template<typename T>
static void conversion(AudioTestContext &Test)
{
   constexpr double scale = std::is_same_v<T, uint8_t> ? 128 : 32768;
   constexpr int offset = std::is_same_v<T, uint8_t> ? 128 : 0;
   constexpr int count = 131072;
   std::vector<float> input(count);
   std::vector<T> enabled(count), disabled(count), repeated(count);
   for (int i = 0; i < count; ++i) input[i] = float(4.1 / scale * std::sin(2 * std::numbers::pi * (i % 256) / 256));
   AudioDitherState state;
   convert_samples<T>(input.data(), count, enabled.data(), &state, 2);
   state.reset();
   // An odd split verifies that the channel cursor survives arbitrary conversion boundaries.
   convert_samples<T>(input.data(), 17, repeated.data(), &state, 2);
   convert_samples<T>(input.data() + 17, count - 17, repeated.data() + 17, &state, 2);
   AUDIO_CHECK(std::memcmp(enabled.data(), repeated.data(), count * sizeof(T)) IS 0);
   convert_samples<T>(input.data(), count, disabled.data());
   double mean = 0, correlation_on = 0, power = 0;
   double phase_on[256]{}, phase_off[256]{};
   bool bounded = true;
   for (int i = 0; i < count; ++i) {
      const double sample = input[i] * scale;
      const double error = int(enabled[i]) - offset - sample;
      mean += error;
      correlation_on += error * sample;
      phase_on[i % 256] += error;
      phase_off[i % 256] += int(disabled[i]) - offset - sample;
      power += sample * sample;
      bounded = bounded and int(enabled[i]) >= offset - scale and int(enabled[i]) <= offset + scale - 1;
   }
   AUDIO_CHECK(bounded and std::abs(mean / count) < 0.01);
   AUDIO_CHECK(std::abs(correlation_on / power) < 0.005); // Average gain within 0.5% at four LSB peak.
   double coherent_on = 0, coherent_off = 0;
   for (int phase = 0; phase < 256; ++phase) {
      coherent_on += phase_on[phase] * phase_on[phase];
      coherent_off += phase_off[phase] * phase_off[phase];
   }
   // Phase averaging measures all signal-correlated distortion, including unsigned truncation's DC bias.
   AUDIO_CHECK(coherent_on < coherent_off * 0.02);

   const float endpoints[] = { -2, -1, 0, 1, 2, float((scale - 1) / scale) };
   T result[6]{};
   convert_samples<T>(endpoints, 6, result, &state, 2);
   AUDIO_CHECK(int(result[0]) IS offset - scale and int(result[1]) IS offset - scale);
   AUDIO_CHECK(int(result[2]) IS offset);
   AUDIO_CHECK(int(result[3]) IS offset + scale - 1 and int(result[4]) IS offset + scale - 1);
   AUDIO_CHECK(int(result[5]) IS offset + scale - 1);

   const auto before = state.State;
   std::fill(input.begin(), input.end(), 0);
   convert_samples<T>(input.data(), count, enabled.data(), &state, 2);
   AUDIO_CHECK(std::all_of(enabled.begin(), enabled.end(), [](T Value) { return int(Value) IS offset; }));
   AUDIO_CHECK(state.State != before);
}

static void float_output(AudioTestContext &Test)
{
   const float input[] = { -2, -1, -0.00001f, -0.0f, 0, 0.00001f, 1, 2 };
   float off[8], on[8];
   AudioDitherState state;
   const auto before = state.State;
   convert_samples<float>(input, 8, off);
   convert_samples<float>(input, 8, on, &state, 2);
   AUDIO_CHECK(std::memcmp(off, on, sizeof(off)) IS 0);
   AUDIO_CHECK(state.State IS before);
}

static void policy_and_lifecycle(AudioTestContext &Test)
{
   extAudio *audio;
   AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
   std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
   SET_Flags(audio, 0);
   AUDIO_REQUIRE(audio->setDither(ADITHER::AUTO) IS ERR::Okay);
   for (int quality : { 0, 9, 10, 32, 33, 65, 66, 100 }) {
      AUDIO_REQUIRE(audio->setQuality(quality) IS ERR::Okay);
      for (auto mode : { ADITHER::AUTO, ADITHER::OFF, ADITHER::TPDF }) {
         AUDIO_REQUIRE(audio->setDither(mode) IS ERR::Okay);
         audio->snapshot_quality();
         AUDIO_CHECK(audio->WindowDither IS (mode IS ADITHER::TPDF or (mode IS ADITHER::AUTO and quality >= 33)));
      }
   }
   audio->DitherState.next(2);
   const auto advanced = audio->DitherState.State;
   AUDIO_REQUIRE(audio->setDither(ADITHER::OFF) IS ERR::Okay);
   audio->snapshot_quality();
   AUDIO_REQUIRE(audio->setDither(ADITHER::AUTO) IS ERR::Okay);
   audio->snapshot_quality();
   AUDIO_CHECK(audio->DitherState.State IS advanced);
   AUDIO_REQUIRE(AUDIO_Deactivate(audio) IS ERR::Okay);
   AudioDitherState initial;
   AUDIO_CHECK(audio->DitherState.State IS initial.State and audio->DitherState.Channel IS 0);
   // Exercise the activation/reopen commit shared by both backends without requiring a device.
   for (const auto layout : { std::span<const int>(glLayoutMono), std::span<const int>(glLayoutStereo) }) {
      audio->DitherState.next(2);
      audio->OutputRate = layout.size() IS 1 ? 44100 : 48000;
      AUDIO_REQUIRE(commit_audio_output(audio, layout, nullptr) IS ERR::Okay);
      AUDIO_CHECK(audio->DitherState.State IS initial.State and audio->DitherState.Channel IS 0);
   }
}

static void run(AudioTestContext &Test)
{
   statistics(Test);
   conversion<uint8_t>(Test);
   conversion<int16_t>(Test);
   float_output(Test);
   policy_and_lifecycle(Test);
}

} // namespace audio_tests_audio_dither
