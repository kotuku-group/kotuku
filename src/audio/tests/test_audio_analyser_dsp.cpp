// Included by audio.cpp to exercise the module implementation.

namespace audio_tests_audio_analyser_dsp {

// Configure exactly as activation does: prepare outside the lock, publish, then reset on the render thread.

struct AnalyserFixture {
   extAudioEffect Effect{nullptr, 1};
   AnalyserProcessor Processor{&Effect};
   ERR Prepared;

   AnalyserFixture(int Rate, int Channels) {
      configure(Rate, Channels);
   }

   void configure(int Rate, int Channels) {
      Effect.OutputRate = Rate;
      Effect.Layout.assign(size_t(Channels), 0);
      Effect.Stereo = Channels IS 2;
      Effect.ResetPending = false;
      std::unique_ptr<AudioEffectConfiguration> config;
      Prepared = Processor.prepare(Rate, Channels IS 2, config);
      if (config) config->publish();
      Processor.reset();
   }

   void process(std::vector<float> &Buffer) {
      Processor.process(Buffer.data(), int(Buffer.size() / Effect.Layout.size()));
   }
};

static std::vector<float> sine(double Frequency, double Amplitude, int Rate, int Frames)
{
   std::vector<float> buffer(size_t(Frames), 0.0f);
   for (int i = 0; i < Frames; i++) {
      buffer[size_t(i)] = float(Amplitude * std::sin(2.0 * std::numbers::pi * Frequency * double(i) / double(Rate)));
   }
   return buffer;
}

// Band levels of Samples, which must be one analysis window long.

static std::vector<double> levels(const std::vector<float> &Samples, int Rate, const std::vector<double> &Edges)
{
   std::vector<double> power, result(Edges.size() - 1);
   analyser_power(Samples, power);
   analyser_bands(power, double(Rate) / double(Samples.size()), Edges, result);
   return result;
}

//********************************************************************************************************************

static void test_sizes(AudioTestContext &Test)
{
   AUDIO_CHECK(analyser_window(8000) IS 256);
   AUDIO_CHECK(analyser_window(44100) IS 2048);
   AUDIO_CHECK(analyser_window(48000) IS 2048);
   AUDIO_CHECK(analyser_window(96000) IS 4096);
   AUDIO_CHECK(analyser_window(192000) IS ANALYSER_MAX_WINDOW);

   for (int rate : { 8000, 44100, 48000, 192000 }) {
      const auto capacity = analyser_capacity(rate);
      AUDIO_CHECK(std::has_single_bit(capacity));
      AUDIO_CHECK(capacity >= size_t(rate * ANALYSER_HISTORY) + ANALYSER_MAX_WAVEFORM);
   }

   for (int rate : { 4000, 7999, 192001, 384000 }) {
      AnalyserFixture fixture(rate, 2);
      AUDIO_CHECK(fixture.Prepared IS ERR::NoSupport);
   }
}

//********************************************************************************************************************
// The FFT against a direct DFT.

static void test_fft(AudioTestContext &Test)
{
   const size_t n = 64;
   std::vector<std::complex<double>> data(n), expected(n);
   uint32_t seed = 7;
   for (auto &value : data) {
      seed = seed * 1664525u + 1013904223u;
      value = { double(seed >> 8) / double(1u << 24) - 0.5, 0 };
   }

   for (size_t k = 0; k < n; k++) {
      std::complex<double> sum = 0;
      for (size_t i = 0; i < n; i++) {
         sum += data[i] * std::polar(1.0, -2.0 * std::numbers::pi * double(k * i) / double(n));
      }
      expected[k] = sum;
   }

   analyser_fft(data);
   double error = 0;
   for (size_t k = 0; k < n; k++) error = std::max(error, std::abs(data[k] - expected[k]));
   AUDIO_CHECK(error < 1e-12);
}

//********************************************************************************************************************
// A sinusoid inside a band reads its peak amplitude in dBFS, and its energy does not leak into distant bands.

static void test_calibration(AudioTestContext &Test)
{
   const int rate = 48000;
   const int window = analyser_window(rate);
   const std::vector<double> edges = { 0, 250, 500, 2000, 4000, 8000, 24000 };

   // Bin-centred: bin 32 is exactly 750 Hz.

   auto result = levels(sine(750, 1.0, rate, window), rate, edges);
   AUDIO_CHECK(std::abs(result[2]) < 0.01);
   AUDIO_CHECK(result[0] < -80 and result[4] < -80 and result[5] < -80);

   result = levels(sine(750, 0.1, rate, window), rate, edges);
   AUDIO_CHECK(std::abs(result[2] + 20) < 0.01);

   // Between bins, with the lobe well inside the band.

   result = levels(sine(1000, 0.5, rate, window), rate, edges);
   AUDIO_CHECK(std::abs(result[2] + 6.0206) < 0.1);

   // A bin-centred sinusoid close to Nyquist is measured by the final band.

   result = levels(sine(23906.25, 1.0, rate, window), rate, edges);
   AUDIO_CHECK(std::abs(result[5]) < 0.01 and result[4] < -80);

   // Silence reads the floor.

   std::vector<float> silence(size_t(window), 0.0f);
   result = levels(silence, rate, edges);
   for (auto level : result) AUDIO_CHECK(level IS ANALYSER_FLOOR_DB);
}

//********************************************************************************************************************
// A band narrower than a bin interpolates instead of reading silence, and follows the level of its neighbourhood.

static void test_narrow_bands(AudioTestContext &Test)
{
   const int rate = 48000;
   const int window = analyser_window(rate);
   const std::vector<double> edges = { 40, 44, 48, 52, 56, 60 };

   auto result = levels(sine(50, 1.0, rate, window), rate, edges);
   for (auto level : result) AUDIO_CHECK(level > -30 and level < 0);

   // With a sinusoid far away, the narrow bands are near the floor.

   result = levels(sine(5000, 1.0, rate, window), rate, edges);
   for (auto level : result) AUDIO_CHECK(level < -80);
}

//********************************************************************************************************************
// Capture: channels are averaged, frames are read oldest first, and unwritten or overwritten frames are silent.

static void test_capture(AudioTestContext &Test)
{
   AnalyserFixture fixture(48000, 2);
   AUDIO_REQUIRE(fixture.Prepared IS ERR::Okay);
   auto &p = fixture.Processor;
   AUDIO_CHECK(p.History.size() IS analyser_capacity(48000));
   AUDIO_CHECK(p.audible(PreciseTime(), 0) IS 0); // Nothing rendered

   std::vector<float> buffer = { 1.0f, 0.0f, 0.5f, 0.5f, -1.0f, 0.0f, 0.25f, 0.75f };
   fixture.process(buffer);
   AUDIO_CHECK(p.Written IS 4 and p.RenderTime > 0);

   // Audio passes through unchanged.
   AUDIO_CHECK(buffer[0] IS 1.0f and buffer[1] IS 0.0f and buffer[4] IS -1.0f);

   float out[6];
   p.copy(4, out);
   AUDIO_CHECK(out[0] IS 0.0f and out[1] IS 0.0f); // Before the first frame
   AUDIO_CHECK(out[2] IS 0.5f and out[3] IS 0.5f and out[4] IS -0.5f and out[5] IS 0.5f);

   p.copy(6, out);
   AUDIO_CHECK(out[0] IS 0.5f and out[3] IS 0.5f and out[4] IS 0.0f and out[5] IS 0.0f); // After the last frame

   // Overwritten frames are silent.

   const auto capacity = p.History.size();
   std::vector<float> fill(capacity * 2, 0.125f);
   fixture.process(fill);
   AUDIO_CHECK(p.Written IS 4 + capacity);
   float wrap[2];
   p.copy(int64_t(p.Written) - int64_t(capacity) + 1, wrap);
   AUDIO_CHECK(wrap[0] IS 0.0f and wrap[1] IS 0.125f);

   // Mono and wider layouts.

   AnalyserFixture mono(44100, 1);
   std::vector<float> single = { 0.5f, -0.25f };
   mono.process(single);
   float mono_out[2];
   mono.Processor.copy(2, mono_out);
   AUDIO_CHECK(mono_out[0] IS 0.5f and mono_out[1] IS -0.25f);

   AnalyserFixture surround(48000, 6);
   std::vector<float> frame = { 0.6f, 0.6f, 0.6f, 0.6f, 0.0f, 0.0f };
   surround.process(frame);
   float surround_out[1];
   surround.Processor.copy(1, surround_out);
   AUDIO_CHECK(std::abs(surround_out[0] - 0.4f) < 1e-6f);
}

//********************************************************************************************************************
// The audible position trails the written position by the device queue and advances in real time between renders.

static void test_audible(AudioTestContext &Test)
{
   AnalyserFixture fixture(48000, 1);
   auto &p = fixture.Processor;
   std::vector<float> buffer(4800, 0.5f);
   fixture.process(buffer);
   const auto rendered = p.RenderTime;

   AUDIO_CHECK(p.audible(rendered, 0) IS 4800);
   AUDIO_CHECK(p.audible(rendered, 0.05) IS 2400);
   AUDIO_CHECK(p.audible(rendered + 25000, 0.05) IS 3600);
   AUDIO_CHECK(p.audible(rendered - 1000, 0.05) IS 2400); // Clock skew never moves the position backwards
   AUDIO_CHECK(p.audible(rendered, 5.0) IS 4800 - 48000); // The queue is capped at ANALYSER_HISTORY

   // Once the queue has been heard, the source having stopped, readings are silent.

   std::vector<float> out(64);
   p.copy(p.audible(rendered + 100000, 0.05), out);
   AUDIO_CHECK(std::all_of(out.begin(), out.end(), [](float Value) { return Value IS 0.0f; }));
   p.copy(p.audible(rendered, 0.05), out);
   AUDIO_CHECK(std::all_of(out.begin(), out.end(), [](float Value) { return Value IS 0.5f; }));
}

//********************************************************************************************************************
// Idle and bypass gaps are silent, without discarding audio that is still queued before a short gap.

static void test_capture_gaps(AudioTestContext &Test)
{
   AnalyserFixture fixture(48000, 1);
   auto &p = fixture.Processor;
   std::vector<float> old_audio(4800, 1.0f), new_audio(480, 0.25f);
   std::vector<float> out(64);
   const int64_t start = 1000000;
   p.process(old_audio.data(), int(old_audio.size()), start);

   // Bypass or an idle chain reports skipped frames.  Previously queued audio is preserved until it is heard.

   p.skip(480, start + 10000);
   p.copy(p.audible(start + 10000, 0.01), out);
   AUDIO_CHECK(std::all_of(out.begin(), out.end(), [](float Value) { return Value IS 1.0f; }));
   p.reset();
   p.process(new_audio.data(), int(new_audio.size()), start + 20000);
   AUDIO_CHECK(p.Written IS 5760);
   p.copy(p.audible(start + 20000, 0.01), out);
   AUDIO_CHECK(std::all_of(out.begin(), out.end(), [](float Value) { return Value IS 0.0f; }));
   p.copy(p.audible(start + 30000, 0.01), out);
   AUDIO_CHECK(std::all_of(out.begin(), out.end(), [](float Value) { return Value IS 0.25f; }));

   // No render calls occur while the device sleeps.  A long idle gap must not resurrect the old audio on resume.

   p.reset();
   p.copy(p.audible(start + 2020000, 0.1), out);
   AUDIO_CHECK(std::all_of(out.begin(), out.end(), [](float Value) { return Value IS 0.0f; }));
   p.process(new_audio.data(), int(new_audio.size()), start + 2020000);
   p.copy(p.audible(start + 2020000, 0.1), out);
   AUDIO_CHECK(std::all_of(out.begin(), out.end(), [](float Value) { return Value IS 0.0f; }));
   p.copy(p.audible(start + 2120000, 0.1), out);
   AUDIO_CHECK(std::all_of(out.begin(), out.end(), [](float Value) { return Value IS 0.25f; }));

   // Skips spanning the ring wrap, including skips larger than the ring, leave only silence in their slots.

   p.skip(int(p.History.size()) + 17, start + 2120000);
   p.copy(int64_t(p.Written), out);
   AUDIO_CHECK(std::all_of(out.begin(), out.end(), [](float Value) { return Value IS 0.0f; }));
   p.reset();
   p.process(new_audio.data(), int(new_audio.size()), start + 2130000);
   p.copy(p.audible(start + 2130000, 0.01), out);
   AUDIO_CHECK(std::all_of(out.begin(), out.end(), [](float Value) { return Value IS 0.0f; }));
}

// Exercise the mixer paths that used to omit idle and bypassed frames entirely.

static void test_mixer_gaps(AudioTestContext &Test)
{
   auto chain = std::make_shared<AudioEffectChain>(std::make_shared<std::recursive_mutex>());
   extAudioEffect effect(nullptr, 1);
   effect.Chain = chain;
   effect.OutputRate = chain->Rate = 48000;
   effect.set_layout(glLayoutMono);
   chain->Layout.assign(std::begin(glLayoutMono), std::end(glLayoutMono));
   chain->Effects.push_back(&effect);
   auto processor = std::make_unique<AnalyserProcessor>(&effect);
   auto p = processor.get();
   AUDIO_REQUIRE(effect.set_processor(std::move(processor)) IS ERR::Okay);

   std::vector<float> audio(4800, 1.0f), out(64);
   render_effects(*chain, audio.data(), 4800, 4800, 48000, true);
   AUDIO_CHECK(p->Written IS 4800);

   effect.Flags = AEF::BYPASS;
   render_effects(*chain, audio.data(), 4800, 4800, 48000, true);
   AUDIO_CHECK(p->Written IS 9600);
   p->copy(int64_t(p->Written), out);
   AUDIO_CHECK(std::all_of(out.begin(), out.end(), [](float Value) { return Value IS 0.0f; }));
   AUDIO_CHECK(audio.front() IS 1.0f and audio.back() IS 1.0f); // Bypass leaves the audio unchanged.

   effect.Flags = AEF::NIL;
   effect.ResetPending = true;
   std::fill(audio.begin(), audio.end(), 0.0f);
   render_effects(*chain, audio.data(), 4800, 0, 48000);
   AUDIO_CHECK(p->Written IS 14400);
   std::fill(audio.begin(), audio.end(), 0.25f);
   render_effects(*chain, audio.data(), 4800, 4800, 48000, true);
   p->copy(p->audible(p->RenderTime, 0.1), out);
   AUDIO_CHECK(std::all_of(out.begin(), out.end(), [](float Value) { return Value IS 0.0f; }));
   p->copy(int64_t(p->Written), out);
   AUDIO_CHECK(std::all_of(out.begin(), out.end(), [](float Value) { return Value IS 0.25f; }));
}

//********************************************************************************************************************
// Reset keeps queued history.  Reconfiguring at the same rate keeps it too, while a new rate discards it.

static void test_reconfiguration(AudioTestContext &Test)
{
   AnalyserFixture fixture(48000, 2);
   auto &p = fixture.Processor;
   std::vector<float> buffer(200, 0.5f);
   fixture.process(buffer);

   p.reset();
   AUDIO_CHECK(p.Written IS 100);

   fixture.configure(48000, 2);
   AUDIO_CHECK(p.Written IS 100 and p.History[0] IS 0.5f);

   fixture.configure(44100, 2);
   AUDIO_CHECK(p.Written IS 0 and p.RenderTime IS 0 and p.Rate IS 44100);
   AUDIO_CHECK(p.History.size() IS analyser_capacity(44100) and p.History[0] IS 0.0f);

   // A processor whose rate differs from the effect's output rate is inactive until reconfigured.

   fixture.Effect.OutputRate = 48000;
   p.reset();
   fixture.process(buffer);
   AUDIO_CHECK(p.Written IS 0);

   // Without a rate, nothing is allocated or captured.

   AnalyserFixture idle(0, 2);
   AUDIO_CHECK(idle.Prepared IS ERR::Okay and idle.Processor.History.empty());
   idle.process(buffer);
   AUDIO_CHECK(idle.Processor.Written IS 0);
   float out[4];
   idle.Processor.copy(4, out);
   AUDIO_CHECK(out[0] IS 0.0f and out[3] IS 0.0f);
}

//********************************************************************************************************************

static void run(AudioTestContext &Test)
{
   test_sizes(Test);
   test_fft(Test);
   test_calibration(Test);
   test_narrow_bands(Test);
   test_capture(Test);
   test_audible(Test);
   test_capture_gaps(Test);
   test_mixer_gaps(Test);
   test_reconfiguration(Test);
}

}
