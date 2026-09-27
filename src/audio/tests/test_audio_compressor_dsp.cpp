// Included by audio.cpp to exercise the module implementation.

#include <chrono>

namespace audio_tests_audio_compressor_dsp {

// Configure exactly as activation does: prepare outside the lock, publish, then reset on the render thread.

struct CompressorFixture {
   extAudioEffect Effect{nullptr, 1};
   CompressorProcessor Processor;
   ERR Prepared;

   CompressorFixture(int Rate, bool Stereo, const CompressorSettings &Settings) : Processor(&Effect, Settings) {
      Effect.OutputRate = Rate;
      Effect.Stereo = Stereo;
      Effect.ResetPending = false;
      std::unique_ptr<AudioEffectConfiguration> config;
      Prepared = Processor.prepare(Rate, Stereo, config);
      if (config) config->publish();
      Processor.reset();
   }

   // Parameter updates derive their target on the control thread, as CompressorUpdate does.
   void change(const CompressorSettings &Settings) {
      CompressorTarget target;
      const bool valid = compressor_target(Settings, Processor.Rate, target);
      Processor.update(Settings, valid ? &target : nullptr);
   }

   void process(std::vector<float> &Buffer) {
      Processor.process(Buffer.data(), int(Buffer.size()) / (Effect.Stereo ? 2 : 1));
   }
};

// A hard knee with near-instant timing, so that steady-state levels follow the static curve.

static CompressorSettings fast_settings()
{
   return CompressorSettings { .Threshold = -20, .Ratio = 4, .Attack = 0.1, .Release = 5, .Knee = 0, .Makeup = 0,
      .Mix = 100 };
}

static double noise(uint32_t &Seed)
{
   Seed = Seed * 1664525u + 1013904223u;
   return double(Seed >> 8) / double(1u << 23) - 1.0;
}

// A constant-magnitude square wave keeps the detector level fixed while still exercising both polarities.

static std::vector<float> square(double Level, int Frames, int Channels)
{
   std::vector<float> buffer(size_t(Frames) * Channels);
   for (int i = 0; i < Frames; i++) {
      for (int c = 0; c < Channels; c++) buffer[size_t(i) * Channels + c] = float((i & 1) ? -Level : Level);
   }
   return buffer;
}

static double db_to_gain(double Db)
{
   return std::pow(10.0, Db / 20.0);
}

//********************************************************************************************************************

static void test_configuration(AudioTestContext &Test)
{
   for (int rate : { 4000, 7999, 192001, 384000 }) {
      CompressorFixture fixture(rate, true, CompressorSettings());
      AUDIO_CHECK(fixture.Prepared IS ERR::NoSupport);
   }

   // Both limits of the supported range compress.
   for (int rate : { 8000, 192000 }) {
      CompressorFixture fixture(rate, false, fast_settings());
      AUDIO_CHECK(fixture.Prepared IS ERR::Okay);
      auto buffer = square(1.0, rate / 10, 1);
      fixture.process(buffer);
      AUDIO_CHECK(fixture.Processor.gain_reduction() > 14.9 and std::abs(buffer.back()) < 0.2);
   }

   // An unconfigured processor passes audio through and reports no reduction.
   CompressorFixture idle(0, true, fast_settings());
   AUDIO_CHECK(idle.Prepared IS ERR::Okay);
   float samples[] = { 0.25f, -0.5f, 2.0f, 1.0f };
   idle.Processor.process(samples, 2);
   AUDIO_CHECK(samples[0] IS 0.25f and samples[1] IS -0.5f and samples[2] IS 2.0f and samples[3] IS 1.0f);
   AUDIO_CHECK(idle.Processor.gain_reduction() IS 0);

   // Targets hold DSP-ready values derived for the rate.
   CompressorTarget target;
   AUDIO_CHECK(not compressor_target(CompressorSettings(), 7999, target));
   AUDIO_REQUIRE(compressor_target(CompressorSettings(), 48000, target));
   AUDIO_CHECK(target.Rate IS 48000 and target.Threshold IS -18 and target.Knee IS 6);
   AUDIO_CHECK(target.Slope IS 0.75 and target.Makeup IS 1 and target.Mix IS 1);
   AUDIO_CHECK(std::abs(target.Attack - std::exp(-1.0 / 480.0)) < 1e-15);
   AUDIO_CHECK(std::abs(target.Release - std::exp(-1.0 / 4800.0)) < 1e-15);
   auto unity = CompressorSettings();
   unity.Ratio = 1;
   unity.Mix = 50;
   unity.Makeup = -6;
   AUDIO_REQUIRE(compressor_target(unity, 48000, target));
   AUDIO_CHECK(target.Slope IS 0 and target.Mix IS 0.5 and std::abs(target.Makeup - 0.501187233627272) < 1e-12);
}

//********************************************************************************************************************
// The static transfer curve against the published equations.

static void test_transfer(AudioTestContext &Test)
{
   for (double ratio : { 1.0, 2.0, 4.0, 20.0 }) {
      const double slope = 1.0 - 1.0 / ratio;
      AUDIO_CHECK(compressor_reduction(-30, -20, 0, slope) IS 0);
      AUDIO_CHECK(compressor_reduction(-20, -20, 0, slope) IS 0);
      AUDIO_CHECK(std::abs(compressor_reduction(-10, -20, 0, slope) - slope * 10) < 1e-12);
      AUDIO_CHECK(std::abs(compressor_reduction(6, -20, 0, slope) - slope * 26) < 1e-12); // Above unity

      // The output level above the knee is T + (x - T) / R.
      const double output = 6 - compressor_reduction(6, -20, 0, slope);
      AUDIO_CHECK(std::abs(output - (-20 + 26 / ratio)) < 1e-12);
   }

   for (double level : { -80.0, -20.0, 0.0, 12.0 }) AUDIO_CHECK(compressor_reduction(level, -20, 12, 0) IS 0);

   // A 12 dB knee centred on -20 dB spans -26 to -14 dB.
   const double slope = 0.75;
   AUDIO_CHECK(compressor_reduction(-26, -20, 12, slope) IS 0);
   AUDIO_CHECK(std::abs(compressor_reduction(-14, -20, 12, slope) - slope * 6) < 1e-12);
   AUDIO_CHECK(std::abs(compressor_reduction(-20, -20, 12, slope) - slope * 36 / 24) < 1e-12);
   AUDIO_CHECK(std::abs(compressor_reduction(-23, -20, 12, slope) - slope * 9 / 24) < 1e-12);
   AUDIO_CHECK(std::abs(compressor_reduction(-17, -20, 12, slope) - slope * 81 / 24) < 1e-12);

   // Value and slope are continuous at both boundaries.
   const double h = 1e-6;
   for (auto [boundary, gradient] : { std::pair { -26.0, 0.0 }, std::pair { -14.0, slope } }) {
      const double below = compressor_reduction(boundary - h, -20, 12, slope);
      const double above = compressor_reduction(boundary + h, -20, 12, slope);
      AUDIO_CHECK(std::abs(above - below) < 1e-5);
      AUDIO_CHECK(std::abs((above - below) / (2 * h) - gradient) < 1e-4);
   }

   // Every supported curve is monotonic and never produces gain.
   bool monotonic = true;
   for (double knee : { 0.0, 6.0, 24.0 }) {
      for (double threshold : { -60.0, -18.0, 0.0 }) {
         double previous = 0;
         for (double level = -100; level <= 24; level += 0.01) {
            const double value = compressor_reduction(level, threshold, knee, 0.95);
            monotonic &= (value >= previous) and (value >= 0);
            previous = value;
         }
      }
   }
   AUDIO_CHECK(monotonic);
}

//********************************************************************************************************************
// Steady-state levels follow the static curve, including internal headroom above unity.

static void test_static_levels(AudioTestContext &Test)
{
   for (int channels = 1; channels <= 2; channels++) {
      for (double level : { 0.05, 0.5, 2.0 }) {
         CompressorFixture fixture(48000, channels IS 2, fast_settings());
         auto buffer = square(level, 24000, channels);
         fixture.process(buffer);
         const double x = 20.0 * std::log10(level);
         const double reduction = compressor_reduction(x, -20, 0, 0.75);
         const double expected = level * db_to_gain(-reduction);
         AUDIO_CHECK(std::abs(fixture.Processor.envelope() - reduction) < 1e-9);
         AUDIO_CHECK(std::abs(std::abs(buffer.back()) - expected) < expected * 1e-6);
         if (reduction > 0) {
            const double output = 20.0 * std::log10(std::abs(double(buffer.back())));
            AUDIO_CHECK(std::abs(output - (-20 + (x + 20) / 4)) < 1e-5);
         }
      }
   }
}

//********************************************************************************************************************
// Ratio 1 with no makeup is sample-identical at every mix, for mono and stereo, below and above unity.

static void test_unity(AudioTestContext &Test)
{
   for (int channels = 1; channels <= 2; channels++) {
      for (double mix : { 0.0, 37.5, 50.0, 100.0 }) {
         auto settings = fast_settings();
         settings.Threshold = -60;
         settings.Knee = 24;
         settings.Ratio = 1;
         settings.Mix = mix;
         CompressorFixture fixture(48000, channels IS 2, settings);
         uint32_t seed = 11;
         std::vector<float> input(4800 * channels);
         for (auto &sample : input) sample = float(noise(seed) * 4.0);
         input[0] = 1.0f;
         input[1] = -1.0f;
         input[2] = 0.0f;
         auto output = input;
         fixture.process(output);
         bool identical = true;
         for (size_t i = 0; i < input.size(); i++) identical &= output[i] IS input[i];
         AUDIO_CHECK(identical);
         AUDIO_CHECK(fixture.Processor.gain_reduction() IS 0);
      }
   }
}

//********************************************************************************************************************
// Zero mix is dry while the detector keeps running; other mixes follow the linear blend; makeup affects only the
// compressed branch.

static void test_mix(AudioTestContext &Test)
{
   uint32_t seed = 5;
   std::vector<float> input(9600 * 2);
   for (auto &sample : input) sample = float(noise(seed) * 0.9);

   auto settings = CompressorSettings();
   settings.Threshold = -30;
   settings.Makeup = 9;

   auto run = [&](double Mix) {
      auto s = settings;
      s.Mix = Mix;
      CompressorFixture fixture(48000, true, s);
      auto buffer = input;
      fixture.process(buffer);
      return std::pair { buffer, fixture.Processor.gain_reduction() };
   };

   const auto [dry, dry_reduction] = run(0);
   const auto [half, half_reduction] = run(50);
   const auto [wet, wet_reduction] = run(100);

   bool identical = true, blend = true;
   for (size_t i = 0; i < input.size(); i++) {
      identical &= dry[i] IS input[i];
      const double expected = double(input[i]) + 0.5 * (double(wet[i]) - double(input[i]));
      blend &= std::abs(half[i] - expected) < 1e-6 * std::max(1.0, std::abs(expected));
   }
   AUDIO_CHECK(identical and blend);
   AUDIO_CHECK(dry_reduction > 1 and dry_reduction IS wet_reduction and half_reduction IS wet_reduction);

   // Raising the mix from zero continues from the envelope that was tracked while dry.
   auto s = settings;
   s.Mix = 0;
   CompressorFixture raised(48000, true, s), reference(48000, true, settings);
   auto a = input, b = input;
   raised.Processor.process(a.data(), 4800);
   reference.Processor.process(b.data(), 4800);
   raised.change(settings);
   raised.Processor.process(a.data() + 9600, 4800);
   reference.Processor.process(b.data() + 9600, 4800);
   bool converged = true;
   for (size_t i = 9600 + 480 * 2; i < a.size(); i++) converged &= a[i] IS b[i];
   AUDIO_CHECK(converged);

   // Makeup limits scale the compressed branch without changing the detector.
   for (double makeup : { -24.0, 24.0 }) {
      auto unity = fast_settings();
      unity.Ratio = 1;
      unity.Makeup = makeup;
      CompressorFixture fixture(48000, false, unity);
      std::vector<float> buffer(input.begin(), input.begin() + 4800);
      fixture.process(buffer);
      bool scaled = true;
      for (size_t i = 0; i < buffer.size(); i++) {
         const double expected = double(input[i]) * db_to_gain(makeup);
         scaled &= std::abs(buffer[i] - expected) < 1e-6 * std::max(1.0, std::abs(expected));
      }
      AUDIO_CHECK(scaled);

      auto compressing = settings;
      compressing.Makeup = makeup;
      auto plain_settings = settings;
      plain_settings.Makeup = 0;
      CompressorFixture boosted(48000, true, compressing), plain(48000, true, plain_settings);
      auto x = input, y = input;
      boosted.process(x);
      plain.process(y);
      AUDIO_CHECK(boosted.Processor.envelope() IS plain.Processor.envelope());
      AUDIO_CHECK(boosted.Processor.gain_reduction() IS plain.Processor.gain_reduction());
   }
}

//********************************************************************************************************************
// Negated input produces exactly negated output, and every combination of parameter limits stays finite.

static void test_polarity_and_range(AudioTestContext &Test)
{
   uint32_t seed = 17;
   std::vector<float> input(4800 * 2);
   for (auto &sample : input) sample = float(noise(seed) * 2.0);

   CompressorFixture positive(48000, true, CompressorSettings()), negative(48000, true, CompressorSettings());
   auto a = input, b = input;
   for (auto &sample : b) sample = -sample;
   positive.process(a);
   negative.process(b);
   bool symmetric = true;
   for (size_t i = 0; i < a.size(); i++) symmetric &= a[i] IS -b[i];
   AUDIO_CHECK(symmetric);

   bool finite = true;
   for (int rate : { 8000, 192000 }) {
      for (int combination = 0; combination < 128; combination++) {
         auto pick = [&](int Bit, double Low, double High) { return (combination & (1 << Bit)) ? High : Low; };
         const CompressorSettings settings {
            .Threshold = pick(0, -60, 0), .Ratio = pick(1, 1, 20), .Attack = pick(2, 0.1, 200),
            .Release = pick(3, 5, 2000), .Knee = pick(4, 0, 24), .Makeup = pick(5, -24, 24), .Mix = pick(6, 0, 100)
         };
         CompressorFixture fixture(rate, true, settings);
         std::vector<float> buffer(2000 * 2);
         for (auto &sample : buffer) sample = float(noise(seed) * 16.0);
         fixture.process(buffer);
         for (auto sample : buffer) finite &= std::isfinite(sample);
         finite &= std::isfinite(fixture.Processor.envelope());
      }
   }
   AUDIO_CHECK(finite);
}

//********************************************************************************************************************
// After one time constant, 1 - 1/e of a step in the target has been traversed during attack, and 1/e remains
// during release.

static void test_timing(AudioTestContext &Test)
{
   for (int rate : { 44100, 48000, 96000 }) {
      const CompressorSettings settings { .Threshold = -40, .Ratio = 4, .Attack = 10, .Release = 100, .Knee = 0,
         .Makeup = 0, .Mix = 100 };
      CompressorFixture fixture(rate, false, settings);
      const double step = compressor_reduction(20.0 * std::log10(0.5), -40, 0, 0.75);
      const double attack_frames = 0.010 * rate, release_frames = 0.100 * rate;

      auto loud = square(0.5, int(std::lround(attack_frames)), 1);
      fixture.process(loud);
      AUDIO_CHECK(std::abs(fixture.Processor.envelope() / step - (1.0 - std::exp(-1.0))) < 1.0 / attack_frames);

      auto settle = square(0.5, rate, 1);
      fixture.process(settle);
      AUDIO_CHECK(std::abs(fixture.Processor.envelope() - step) < 1e-9);

      std::vector<float> silence(size_t(std::lround(release_frames)), 0.0f);
      fixture.process(silence);
      AUDIO_CHECK(std::abs(fixture.Processor.envelope() / step - std::exp(-1.0)) < 1.0 / release_frames);

      // The releasing envelope eventually snaps to exactly zero.
      std::vector<float> long_silence(size_t(rate) * 5, 0.0f);
      fixture.process(long_silence);
      AUDIO_CHECK(fixture.Processor.envelope() IS 0);
      std::vector<float> after(64, 0.0f);
      fixture.process(after);
      AUDIO_CHECK(fixture.Processor.gain_reduction() IS 0);
   }
}

//********************************************************************************************************************
// Without lookahead, the leading edge of a transient passes almost unattenuated and in its original position.

static void test_transient(AudioTestContext &Test)
{
   const CompressorSettings settings { .Threshold = -30, .Ratio = 10, .Attack = 10, .Release = 100, .Knee = 0,
      .Makeup = 0, .Mix = 100 };
   CompressorFixture fixture(48000, false, settings);
   std::vector<float> buffer(2000, 0.0f);
   for (int i = 100; i < 105; i++) buffer[i] = (i & 1) ? -1.0f : 1.0f;
   fixture.process(buffer);

   int peak = 0;
   for (int i = 0; i < int(buffer.size()); i++) if (std::abs(buffer[i]) > std::abs(buffer[peak])) peak = i;
   AUDIO_CHECK(peak IS 100);
   AUDIO_CHECK(std::abs(buffer[100]) > 0.99);                      // The static curve would give -27 dB
   AUDIO_CHECK(fixture.Processor.gain_reduction() < 1.0);
   AUDIO_CHECK(fixture.Processor.latency() IS 0);
   for (int i = 105; i < int(buffer.size()); i++) AUDIO_CHECK(buffer[i] IS 0);
}

//********************************************************************************************************************
// The louder channel controls both channels; mono follows the same curve without a second channel.

static void test_linking(AudioTestContext &Test)
{
   const int frames = 9600;
   auto make = [&](bool Swap, double LoudSign) {
      std::vector<float> buffer(frames * 2);
      for (int i = 0; i < frames; i++) {
         const double loud = LoudSign * 0.8 * std::sin(2.0 * std::numbers::pi * 110.0 * i / 48000.0);
         const double quiet = 0.01 * std::sin(2.0 * std::numbers::pi * 1000.0 * i / 48000.0 + 0.3);
         buffer[i * 2] = float(Swap ? quiet : loud);
         buffer[i * 2 + 1] = float(Swap ? loud : quiet);
      }
      return buffer;
   };

   auto gains = [&](bool Swap, double LoudSign, bool &Linked) {
      CompressorFixture fixture(48000, true, CompressorSettings());
      const auto input = make(Swap, LoudSign);
      auto output = input;
      fixture.process(output);
      std::vector<double> result(frames, 1.0);
      for (int i = 0; i < frames; i++) {
         const double left = double(output[i * 2]) / double(input[i * 2]);
         const double right = double(output[i * 2 + 1]) / double(input[i * 2 + 1]);
         if ((input[i * 2] != 0) and (input[i * 2 + 1] != 0)) {
            Linked &= std::abs(left - right) < 1e-5;
            result[i] = left;
         }
      }
      return result;
   };

   bool linked = true;
   const auto base = gains(false, 1, linked);
   const auto swapped = gains(true, 1, linked);
   const auto inverted = gains(false, -1, linked);
   AUDIO_CHECK(linked);
   bool same = true, compressed = false;
   for (int i = 0; i < frames; i++) {
      same &= (std::abs(base[i] - swapped[i]) < 1e-6) and (std::abs(base[i] - inverted[i]) < 1e-6);
      compressed |= base[i] < 0.5;
   }
   AUDIO_CHECK(same and compressed);

   // A silent right channel leaves the left identical to mono processing.
   CompressorFixture stereo(48000, true, CompressorSettings()), mono(48000, false, CompressorSettings());
   auto pair = make(false, 1);
   std::vector<float> single(frames);
   for (int i = 0; i < frames; i++) {
      pair[i * 2 + 1] = 0;
      single[i] = pair[i * 2];
   }
   stereo.process(pair);
   mono.process(single);
   bool identical = true;
   for (int i = 0; i < frames; i++) identical &= (pair[i * 2] IS single[i]) and (pair[i * 2 + 1] IS 0);
   AUDIO_CHECK(identical);
}

//********************************************************************************************************************
// Block boundaries do not change the output, the final state or the maximum reduction, including parameter edits
// during a ramp.

static void test_blocks(AudioTestContext &Test)
{
   uint32_t seed = 3;
   std::vector<float> input(19200 * 2);
   for (int i = 0; i < 19200; i++) {
      const double envelope = (i / 2400) & 1 ? 0.9 : 0.05;
      input[i * 2] = float(noise(seed) * envelope);
      input[i * 2 + 1] = float(noise(seed) * envelope);
   }

   CompressorSettings edits[3] = { CompressorSettings(), CompressorSettings(), CompressorSettings() };
   edits[0].Threshold = -40;
   edits[0].Makeup = 12;
   edits[0].Knee = 0;
   edits[1].Ratio = 20;          // During the first ramp
   edits[1].Attack = 1;
   edits[1].Mix = 40;
   edits[2].Release = 20;        // Rapid replacement of the endpoint
   edits[2].Knee = 24;

   auto render = [&](int Pattern, std::array<double, 4> &Maximum, double &Envelope) {
      static const int irregular[] = { 1, 7, 64, 3, 129, 480, 2 };
      CompressorFixture fixture(48000, true, CompressorSettings());
      auto buffer = input;
      int position = 0, cycle = 0;
      const int stops[] = { 4800, 5000, 5100, 19200 };
      for (int stage = 0; stage < 4; stage++) {
         Maximum[stage] = 0;
         while (position < stops[stage]) {
            int block = stops[stage] - position;
            if (Pattern IS 1) block = 1;
            else if (Pattern IS 2) block = std::min(block, irregular[cycle++ % std::size(irregular)]);
            fixture.Processor.process(buffer.data() + position * 2, block);
            Maximum[stage] = std::max(Maximum[stage], fixture.Processor.gain_reduction());
            position += block;
         }
         if (stage < 3) fixture.change(edits[stage]);
      }
      Envelope = fixture.Processor.envelope();
      return buffer;
   };

   std::array<double, 4> whole_max, single_max, irregular_max;
   double whole_envelope, single_envelope, irregular_envelope;
   const auto whole = render(0, whole_max, whole_envelope);
   const auto single = render(1, single_max, single_envelope);
   const auto irregular = render(2, irregular_max, irregular_envelope);

   bool identical = true, finite = true;
   for (size_t i = 0; i < whole.size(); i++) {
      identical &= (whole[i] IS single[i]) and (whole[i] IS irregular[i]);
      finite &= std::isfinite(whole[i]);
   }
   AUDIO_CHECK(identical and finite);
   AUDIO_CHECK(whole_envelope IS single_envelope and whole_envelope IS irregular_envelope);
   AUDIO_CHECK(whole_max IS single_max and whole_max IS irregular_max);
   AUDIO_CHECK(whole_max[3] > 5);
}

//********************************************************************************************************************
// Live edits to every parameter, including edits during a ramp, change the applied gain gradually and converge to
// the latest settings.

static void test_automation(AudioTestContext &Test)
{
   const int frames = 96000;
   const auto input = square(0.5, frames, 2);
   auto output = input;
   CompressorFixture fixture(48000, true, CompressorSettings());

   const CompressorSettings first { .Threshold = -40, .Ratio = 10, .Attack = 1, .Release = 50, .Knee = 0,
      .Makeup = 12, .Mix = 70 };
   const CompressorSettings second { .Threshold = -10, .Ratio = 2, .Attack = 50, .Release = 500, .Knee = 24,
      .Makeup = -6, .Mix = 30 };
   const CompressorSettings last { .Threshold = -30, .Ratio = 8, .Attack = 5, .Release = 200, .Knee = 12,
      .Makeup = 6, .Mix = 90 };

   fixture.Processor.process(output.data(), 24000);
   fixture.change(first);
   fixture.Processor.process(output.data() + 24000 * 2, 200);
   fixture.change(second);
   fixture.Processor.process(output.data() + 24200 * 2, 50);
   fixture.change(last);
   fixture.Processor.process(output.data() + 24250 * 2, frames - 24250);

   // The applied gain is the output/input ratio of the constant-magnitude input.
   double largest = 0;
   bool linked = true;
   for (int i = 1; i < frames; i++) {
      const double gain = double(output[i * 2]) / double(input[i * 2]);
      const double previous = double(output[(i - 1) * 2]) / double(input[(i - 1) * 2]);
      linked &= output[i * 2] IS output[i * 2 + 1];
      largest = std::max(largest, std::abs(gain - previous));
   }
   AUDIO_CHECK(linked);
   AUDIO_CHECK(largest < 0.01); // An unramped makeup change of 12 dB at 70% mix would step by 1.4

   // The ramp converges on the latest settings, identical to a compressor that began with them.
   CompressorFixture reference(48000, true, last);
   auto expected = input;
   reference.process(expected);
   AUDIO_CHECK(std::abs(fixture.Processor.envelope() - reference.Processor.envelope()) < 1e-9);
   AUDIO_CHECK(std::abs(output.back() - expected.back()) < 1e-6);
}

//********************************************************************************************************************
// Reset discards reduction and ramps and adopts the latest settings.  The compressor never has a tail.

static void test_reset(AudioTestContext &Test)
{
   auto heavy = CompressorSettings();
   heavy.Threshold = -60;
   heavy.Ratio = 20;
   heavy.Release = 2000;
   CompressorFixture fixture(48000, true, heavy);
   auto buffer = square(4.0, 48000, 2);
   fixture.process(buffer);
   AUDIO_CHECK(fixture.Processor.envelope() > 60);

   AUDIO_CHECK(not fixture.Processor.pending());
   AUDIO_CHECK(fixture.Processor.tail() IS AudioTail::NONE);
   AUDIO_CHECK(fixture.Processor.tail_frames() IS 0 and fixture.Processor.decay_estimate() IS 0);
   AUDIO_CHECK(fixture.Processor.latency() IS 0);
   std::unique_ptr<AudioEffectConfiguration> config;
   AUDIO_CHECK(fixture.Processor.prepare(48000, true, config) IS ERR::Okay and config and config->latency() IS 0);

   // An edit that is still ramping is cancelled by reset, which applies its endpoint immediately.
   auto edited = heavy;
   edited.Threshold = -12;
   edited.Makeup = 6;
   fixture.change(edited);
   auto partial = square(0.5, 100, 2);
   fixture.process(partial);
   fixture.Processor.reset();
   AUDIO_CHECK(fixture.Processor.envelope() IS 0 and fixture.Processor.gain_reduction() IS 0);

   CompressorFixture fresh(48000, true, edited);
   auto a = square(0.5, 4800, 2), b = a;
   fixture.process(a);
   fresh.process(b);
   bool identical = true;
   for (size_t i = 0; i < a.size(); i++) identical &= a[i] IS b[i];
   AUDIO_CHECK(identical);

   // Edits while the output is unconfigured are adopted by the next configuration.
   CompressorFixture unconfigured(0, false, CompressorSettings());
   unconfigured.change(edited);
   unconfigured.Effect.OutputRate = 48000;
   std::unique_ptr<AudioEffectConfiguration> late;
   AUDIO_REQUIRE(unconfigured.Processor.prepare(48000, false, late) IS ERR::Okay);
   late->publish();
   unconfigured.Processor.reset();
   CompressorFixture mono(48000, false, edited);
   auto c = square(0.5, 4800, 1), d = c;
   unconfigured.process(c);
   mono.process(d);
   identical = true;
   for (size_t i = 0; i < c.size(); i++) identical &= c[i] IS d[i];
   AUDIO_CHECK(identical);
}

//********************************************************************************************************************
// gain_reduction() reports the maximum envelope of the most recent call, reads are non-destructive, and every call
// starts from zero.

static void test_reduction_meter(AudioTestContext &Test)
{
   auto settings = fast_settings();
   settings.Release = 50;
   CompressorFixture fixture(48000, false, settings);

   std::vector<float> buffer = square(1.0, 4800, 1);
   buffer.resize(9600, 0.0f);
   fixture.process(buffer);
   const double reduction = fixture.Processor.gain_reduction();
   AUDIO_CHECK(std::abs(reduction - 15) < 1e-6);
   AUDIO_CHECK(fixture.Processor.envelope() < reduction * 0.5);
   AUDIO_CHECK(fixture.Processor.gain_reduction() IS reduction);

   std::vector<float> silence(480, 0.0f);
   const double before = fixture.Processor.envelope();
   fixture.process(silence);
   AUDIO_CHECK(fixture.Processor.gain_reduction() < before and fixture.Processor.gain_reduction() > 0);

   std::vector<float> quiet(48000 * 2, 0.001f);
   fixture.process(quiet);
   fixture.process(silence);
   AUDIO_CHECK(fixture.Processor.gain_reduction() IS 0);
}

//********************************************************************************************************************
// Integration with extAudioEffect: meter layout, interval aggregation, bypass, idle and leaving bypass.

struct ChainFixture {
   std::shared_ptr<std::recursive_mutex> Mutex = std::make_shared<std::recursive_mutex>();
   std::shared_ptr<AudioEffectChain> Chain = std::make_shared<AudioEffectChain>(Mutex);
   extAudioEffect Effects[2] { { nullptr, 0 }, { nullptr, 0 } };
   int Count = 0;

   ChainFixture(int Rate, int Channels) {
      Chain->Rate = Rate;
      Chain->Stereo = Channels IS 2;
      const auto layout = Channels IS 2 ? std::span<const int>(glLayoutStereo) : std::span<const int>(glLayoutMono);
      Chain->Layout.assign(layout.begin(), layout.end());
   }

   ERR add(std::unique_ptr<AudioEffectProcessor> Processor, const AudioEffectSchema *Schema = nullptr) {
      auto &effect = Effects[Count++];
      effect.Chain = Chain;
      effect.Schema = Schema;
      effect.OutputRate = Chain->Rate;
      effect.set_layout(Chain->Layout);
      Chain->Effects.push_back(&effect);
      return effect.set_processor(std::move(Processor));
   }
};

static void test_effect_integration(AudioTestContext &Test)
{
   ChainFixture mono(48000, 1);
   auto processor = std::make_unique<CompressorProcessor>(&mono.Effects[0], fast_settings());
   AUDIO_REQUIRE(mono.add(std::move(processor), &glCompressorSchema) IS ERR::Okay);
   auto &mono_effect = mono.Effects[0];
   AUDIO_REQUIRE(mono_effect.Meters.size() IS 3);
   AUDIO_CHECK(std::string_view(mono_effect.Meters[0].Key) IS "input_peak_centre");
   AUDIO_CHECK(std::string_view(mono_effect.Meters[1].Key) IS "output_peak_centre");
   AUDIO_CHECK(std::string_view(mono_effect.Meters[2].Key) IS "gain_reduction");

   ChainFixture fixture(48000, 2);
   auto owned = std::make_unique<CompressorProcessor>(&fixture.Effects[0], fast_settings());
   auto compressor = owned.get();
   AUDIO_REQUIRE(fixture.add(std::move(owned), &glCompressorSchema) IS ERR::Okay);
   auto &effect = fixture.Effects[0];
   AUDIO_REQUIRE(effect.Meters.size() IS 5);
   const char *keys[] = { "input_peak_left", "input_peak_right", "output_peak_left", "output_peak_right",
      "gain_reduction" };
   for (int i = 0; i < 5; i++) AUDIO_CHECK(std::string_view(effect.Meters[i].Key) IS keys[i]);
   AUDIO_CHECK(std::string_view(effect.Meters[4].Scope) IS "global");
   AUDIO_CHECK(std::string_view(effect.Meters[4].Semantics) IS "maximum-attenuation");
   AUDIO_CHECK(effect.Meters[4].Channel IS 0 and std::string_view(effect.Meters[4].Unit) IS "dB");
   AUDIO_CHECK(effect.Meter.Values[4] IS 0 and effect.Meter.ValueFlags[4] IS 0);

   const auto xml = build_schema_xml(glCompressorSchema, effect.Meters);
   AUDIO_CHECK(xml.find("key=\"gain_reduction\" type=\"scalar\"") != std::string::npos);
   AUDIO_CHECK(xml.find("slot=\"4\"") != std::string::npos);

   // One 50 ms interval of a loud left channel, processed in uneven blocks.  The reduction is the maximum across
   // every call in the interval; the output peak is reduced by the static curve.
   auto loud = square(1.0, 2400, 2);
   for (int i = 0; i < 2400; i++) loud[i * 2 + 1] *= 0.25f;
   int position = 0;
   for (int block : { 7, 1000, 393, 1000 }) {
      effect.process(loud.data() + position * 2, block);
      position += block;
   }
   AUDIO_CHECK(effect.Meter.Sequence IS 1 and effect.Meter.Interval IS 2400);
   AUDIO_CHECK((effect.Meter.Flags & AMF::VALID) != AMF::NIL);
   AUDIO_CHECK(std::abs(effect.Meter.Values[0]) < 1e-9);
   AUDIO_CHECK(std::abs(effect.Meter.Values[1] - 20.0 * std::log10(0.25)) < 1e-6);
   // The output peak occurs on the first frames, before the attack has taken effect, and is linked across channels.
   AUDIO_CHECK(effect.Meter.Values[2] < -1 and effect.Meter.Values[2] > -15);
   AUDIO_CHECK(std::abs(effect.Meter.Values[3] - (effect.Meter.Values[2] + 20.0 * std::log10(0.25))) < 1e-5);
   AUDIO_CHECK(std::abs(effect.Meter.Values[4] - 15) < 1e-6);
   AUDIO_CHECK(effect.Meter.ValueFlags[4] IS int(AMV::VALID));

   // During release the interval reports its greatest reduction, not the final value.
   std::vector<float> silence(2400 * 2, 0.0f);
   effect.process(silence.data(), 2400);
   AUDIO_CHECK(effect.Meter.Sequence IS 2);
   AUDIO_CHECK(effect.Meter.Values[4] > 14 and compressor->envelope() < 1e-2);

   // Bypass leaves the buffer untouched; leaving bypass hard-resets the envelope.  Buffers are processed in place,
   // so every stage uses fresh input.
   auto again = square(1.0, 1200, 2);
   effect.process(again.data(), 1200);
   AUDIO_CHECK(compressor->envelope() > 14);
   effect.Channel = 1 << 16;
   AUDIO_CHECK(AUDIOEFFECT_SET_Flags(&effect, AEF::BYPASS) IS ERR::Okay);
   auto bypassed = square(1.0, 480, 2), original = bypassed;
   effect.process(bypassed.data(), 480);
   AUDIO_CHECK(bypassed IS original);
   AUDIO_CHECK(AUDIOEFFECT_SET_Flags(&effect, AEF::NIL) IS ERR::Okay);
   std::array<float, 2> frame = { 0.001f, 0.001f };
   effect.process(frame.data(), 1);
   AUDIO_CHECK(compressor->envelope() IS 0 and frame[0] IS 0.001f);

   // Idle discards the envelope.
   again = square(1.0, 1200, 2);
   effect.process(again.data(), 1200);
   AUDIO_CHECK(compressor->envelope() > 14);
   effect.idle();
   AUDIO_CHECK(compressor->envelope() IS 0 and compressor->gain_reduction() IS 0);
   AUDIO_CHECK((effect.Meter.Flags & AMF::IDLE) != AMF::NIL);
   AUDIO_CHECK(not effects_pending(*fixture.Chain));
}

//********************************************************************************************************************
// The threshold is absolute after a linear processor, and a compressor that has no tail does not truncate a
// downstream reverb.

static void test_chains(AudioTestContext &Test)
{
   uint32_t seed = 23;
   std::vector<float> input(4800 * 2);
   for (auto &sample : input) sample = float(noise(seed) * 0.3);

   // An equaliser trim of +6.02 dB doubles the compressor's input.
   ChainFixture eq(48000, 2);
   AUDIO_REQUIRE(eq.add(std::make_unique<EqualiserProcessor>(&eq.Effects[0], std::vector<AudioEQBand> {},
      6.020599913279624)) IS ERR::Okay);
   AUDIO_REQUIRE(eq.add(std::make_unique<CompressorProcessor>(&eq.Effects[1], CompressorSettings())) IS ERR::Okay);
   auto chained = input;
   render_effects(*eq.Chain, chained.data(), 4800, 4800, 48000);

   CompressorFixture direct(48000, true, CompressorSettings());
   auto doubled = input;
   for (auto &sample : doubled) sample *= 2.0f;
   direct.process(doubled);
   bool matches = true;
   for (size_t i = 0; i < input.size(); i++) matches &= std::abs(chained[i] - doubled[i]) < 1e-6;
   AUDIO_CHECK(matches);

   // Compressor -> reverb: the chain's tail is the reverb's and drains after the source stops.
   ChainFixture tail(48000, 2);
   AUDIO_REQUIRE(tail.add(std::make_unique<CompressorProcessor>(&tail.Effects[0], CompressorSettings())) IS ERR::Okay);
   ReverbSettings reverb_settings;
   reverb_settings.Decay = 300;
   reverb_settings.Mix = 50;
   auto reverb = std::make_unique<ReverbProcessor>(&tail.Effects[1], reverb_settings);
   auto reverb_pointer = reverb.get();
   AUDIO_REQUIRE(tail.add(std::move(reverb)) IS ERR::Okay);

   auto source = input;
   render_effects(*tail.Chain, source.data(), 4800, 4800, 48000 * 10, true);
   AUDIO_CHECK(tail.Chain->tail_bound() IS reverb_pointer->tail_frames());
   AUDIO_CHECK(tail.Chain->decay_estimate() IS reverb_pointer->decay_estimate());

   std::vector<float> drain(4800 * 2, 0.0f);
   render_effects(*tail.Chain, drain.data(), 4800, 0, 48000 * 10);
   double energy = 0;
   for (auto sample : drain) energy += double(sample) * sample;
   AUDIO_CHECK(energy > 1e-3 and tail.Chain->State IS ADS::DRAINING);

   int periods = 0;
   while ((tail.Chain->State != ADS::IDLE) and (periods++ < 100)) {
      std::fill(drain.begin(), drain.end(), 0.0f);
      render_effects(*tail.Chain, drain.data(), 4800, 0, 48000 * 10);
   }
   AUDIO_CHECK(tail.Chain->State IS ADS::IDLE and not tail.Chain->Truncated);
}

//********************************************************************************************************************
// Equivalent signals rendered to 8-bit, 16-bit and float destinations share the same pre-quantisation threshold,
// reduction and meters.

#ifdef AUDIO_WORKER
struct FormatResult {
   std::vector<float> Output;
   double Reduction = -1;
   double InputMeter = 0;
};

static FormatResult render_format(AudioTestContext &Test, int BitDepth, const std::vector<int16_t> &Source)
{
   const int frames = int(Source.size());
   extAudio *audio;
   if (!AUDIO_CHECK(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay)) return {};
   std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
   if (!AUDIO_CHECK(InitObject(audio) IS ERR::Okay)) return {};
   audio->Flags = ADF::NIL;
   audio->OutputRate = 8000;
   audio->BitDepth = BitDepth;
   audio->DriverBitSize = BitDepth / 8;
   audio->Stereo = false;
   audio->MasterVolume = 1;
   audio->MixElements = SAMPLE(frames);
   audio->MixBuffer.resize(frames);
   audio->MixConfig = AudioConfig(false, false);
   audio->Samples.resize(2);
   auto &sample = audio->Samples[1];
   sample.SampleType = PCM::S16_MONO;
   sample.FrameBytes = 2;
   sample.SampleLength = SAMPLE(frames);
   sample.Data.resize(Source.size() * sizeof(int16_t));
   std::memcpy(sample.Data.data(), Source.data(), sample.Data.size());

   audio->Sets.resize(2);
   auto &set = audio->Sets[1];
   set.Channel.resize(1);
   auto &channel = set.Channel[0];
   channel.SampleHandle = 1;
   channel.Frequency = 8000;
   channel.State = CHS::PLAYING;
   channel.LVolume = channel.RVolume = 1;
   channel.Handle = 1 << 16;
   set.Effects = std::make_shared<AudioEffectChain>(audio->MixerLock);
   set.Effects->Generation = audio->GlobalEffects->Generation;
   set.ScratchBuffer.resize(frames);

   extAudioEffect application(nullptr, 0);
   application.Chain = set.Effects;
   application.Schema = &glCompressorSchema;
   application.OutputRate = 8000;
   set.Effects->Layout = audio->GlobalEffects->Layout = { int(SPK::CENTRE) };
   application.set_layout(glLayoutMono);
   set.Effects->Effects.push_back(&application);
   set.Effects->Rate = audio->GlobalEffects->Rate = 8000;

   auto processor = std::make_unique<CompressorProcessor>(&application, fast_settings());
   if (!AUDIO_CHECK(application.set_processor(std::move(processor)) IS ERR::Okay)) return {};

   std::vector<uint8_t> output(size_t(frames) * 4);
   audio->EffectConfigured = true;
   if (!AUDIO_CHECK(mix_data(audio, frames, output.data()) IS ERR::Okay)) return {};
   application.idle();

   FormatResult result;
   result.InputMeter = application.Meter.Values[0];
   result.Reduction = application.Meter.Values[2];
   result.Output.resize(frames);
   for (int i = 0; i < frames; ++i) {
      if (BitDepth IS 8) result.Output[i] = (float(output[i]) - 128.0f) / 128.0f;
      else if (BitDepth IS 16) result.Output[i] = float(((int16_t *)output.data())[i]) / 32768.0f;
      else result.Output[i] = ((float *)output.data())[i];
   }
   return result;
}

static void test_formats(AudioTestContext &Test)
{
   // A quiet passage followed by a loud one, all within a single meter interval.
   std::vector<int16_t> source(320);
   for (int i = 0; i < 320; i++) source[i] = int16_t(((i & 1) ? -1 : 1) * (i < 80 ? 1638 : 29491));

   CompressorFixture reference(8000, false, fast_settings());
   std::vector<float> expected(source.size());
   for (size_t i = 0; i < source.size(); i++) expected[i] = float(source[i]) / 32768.0f;
   reference.process(expected);

   const auto result8 = render_format(Test, 8, source);
   const auto result16 = render_format(Test, 16, source);
   const auto result32 = render_format(Test, 32, source);
   const std::pair<const FormatResult *, double> results[] = {
      { &result8, 1.0 / 128.0 }, { &result16, 1.0 / 32768.0 }, { &result32, 1e-6 }
   };
   for (auto [result, tolerance] : results) {
      AUDIO_REQUIRE(result->Output.size() IS expected.size());
      bool close = true;
      for (size_t i = 0; i < expected.size(); i++) close &= std::abs(result->Output[i] - expected[i]) <= tolerance;
      AUDIO_CHECK(close);
      AUDIO_CHECK(result->Reduction > 7 and std::abs(result->Reduction - result32.Reduction) < 1e-9);
      AUDIO_CHECK(std::abs(result->InputMeter - result32.InputMeter) < 1e-9);
   }
}
#endif

//********************************************************************************************************************
// Throughput is recorded for comparison between builds; it is not a pass/fail criterion.

static void report_throughput()
{
   for (int channels = 1; channels <= 2; channels++) {
      CompressorFixture fixture(48000, channels IS 2, CompressorSettings());
      uint32_t seed = 1;
      std::vector<float> buffer(48000 * channels);
      for (auto &sample : buffer) sample = float(noise(seed) * 0.8);
      const auto start = std::chrono::steady_clock::now();
      for (int second = 0; second < 10; second++) fixture.process(buffer);
      const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      kt::Log("AudioTests").msg("Compressor %s: %.0fx real-time at 48 kHz.", channels IS 2 ? "stereo" : "mono",
         10.0 / elapsed);
   }
}

//********************************************************************************************************************

static void run(AudioTestContext &Test)
{
   test_configuration(Test);
   test_transfer(Test);
   test_static_levels(Test);
   test_unity(Test);
   test_mix(Test);
   test_polarity_and_range(Test);
   test_timing(Test);
   test_transient(Test);
   test_linking(Test);
   test_blocks(Test);
   test_automation(Test);
   test_reset(Test);
   test_reduction_meter(Test);
   test_effect_integration(Test);
   test_chains(Test);
#ifdef AUDIO_WORKER
   test_formats(Test);
#endif
   report_throughput();
}

} // namespace audio_tests_audio_compressor_dsp
