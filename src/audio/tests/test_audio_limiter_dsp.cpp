// Included by audio.cpp to exercise the module implementation.

#include <chrono>

namespace audio_tests_audio_limiter_dsp {

// Configure exactly as activation does: prepare outside the lock, publish, then reset on the render thread.

struct LimiterFixture {
   extAudioEffect Effect{nullptr, 1};
   LimiterProcessor Processor;
   ERR Prepared;

   LimiterFixture(int Rate, bool Stereo, const LimiterSettings &Settings) : Processor(&Effect, Settings) {
      Effect.OutputRate = Rate;
      Effect.Stereo = Stereo;
      Effect.ResetPending = false;
      std::unique_ptr<AudioEffectConfiguration> config;
      Prepared = Processor.prepare(Rate, Stereo, config);
      if (config) config->publish();
      Processor.reset();
   }

   // Parameter updates derive their target on the control thread, as LimiterUpdate does.
   void change(const LimiterSettings &Settings) {
      LimiterTarget target;
      const bool valid = limiter_target(Settings, Processor.Rate, target);
      Processor.update(Settings, valid ? &target : nullptr);
   }

   int channels() const { return Effect.Stereo ? 2 : 1; }

   void process(std::vector<float> &Buffer) {
      Processor.process(Buffer.data(), int(Buffer.size()) / channels());
   }
};

static double noise(uint32_t &Seed)
{
   Seed = Seed * 1664525u + 1013904223u;
   return double(Seed >> 8) / double(1u << 23) - 1.0;
}

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

// The represented ceiling for a setting: the largest float that does not exceed the exact linear ceiling.

static double float_ceiling(double Db)
{
   return limiter_float_floor(db_to_gain(Db));
}

static double peak_of(const std::vector<float> &Buffer, size_t Start = 0)
{
   double peak = 0;
   for (size_t i = Start; i < Buffer.size(); i++) peak = std::max(peak, std::abs(double(Buffer[i])));
   return peak;
}

//********************************************************************************************************************

static void test_configuration(AudioTestContext &Test)
{
   for (int rate : { 4000, 7999, 192001, 384000 }) {
      LimiterFixture fixture(rate, true, LimiterSettings());
      AUDIO_CHECK(fixture.Prepared IS ERR::NoSupport);
      AUDIO_CHECK(fixture.Processor.latency() IS 0);
   }

   // 5 ms rounded up to whole frames.
   AUDIO_CHECK(limiter_lookahead(8000) IS 40 and limiter_lookahead(22050) IS 111);
   AUDIO_CHECK(limiter_lookahead(44100) IS 221 and limiter_lookahead(48000) IS 240);
   AUDIO_CHECK(limiter_lookahead(96000) IS 480 and limiter_lookahead(192000) IS 960);
   AUDIO_CHECK(limiter_lookahead(44101) IS 221 and limiter_lookahead(44200) IS 221 and limiter_lookahead(44201) IS 222);

   // Both limits of the supported range limit; storage is sized by the channel count.
   for (int rate : { 8000, 192000 }) {
      for (int channels = 1; channels <= 2; channels++) {
         LimiterFixture fixture(rate, channels IS 2, LimiterSettings());
         AUDIO_CHECK(fixture.Prepared IS ERR::Okay);
         const int lookahead = limiter_lookahead(rate);
         AUDIO_CHECK(fixture.Processor.latency() IS lookahead);
         AUDIO_CHECK(fixture.Processor.delay_samples() IS size_t(lookahead) * channels);
         AUDIO_CHECK(fixture.Processor.peak_capacity() IS size_t(lookahead) + 1);
         auto buffer = square(2.0, rate / 10, channels);
         fixture.process(buffer);
         AUDIO_CHECK(fixture.Processor.gain_reduction() > 7 and peak_of(buffer) <= float_ceiling(-1));
      }
   }

   // Prepared storage and latency only take effect on publication.
   LimiterFixture fixture(0, true, LimiterSettings());
   std::unique_ptr<AudioEffectConfiguration> config;
   AUDIO_REQUIRE(fixture.Processor.prepare(44100, true, config) IS ERR::Okay and config);
   AUDIO_CHECK(config->latency() IS 221);
   AUDIO_CHECK(fixture.Processor.latency() IS 0 and fixture.Processor.delay_samples() IS 0);
   config->publish();
   AUDIO_CHECK(fixture.Processor.latency() IS 221 and fixture.Processor.delay_samples() IS 442);

   std::unique_ptr<AudioEffectConfiguration> unconfigured;
   AUDIO_REQUIRE(fixture.Processor.prepare(0, true, unconfigured) IS ERR::Okay and unconfigured);
   AUDIO_CHECK(unconfigured->latency() IS 0);

   // An unconfigured processor passes audio through and reports no reduction or latency.
   LimiterFixture idle(0, true, LimiterSettings());
   AUDIO_CHECK(idle.Prepared IS ERR::Okay);
   float samples[] = { 0.25f, -0.5f, 2.0f, 1.0f };
   idle.Processor.process(samples, 2);
   AUDIO_CHECK(samples[0] IS 0.25f and samples[1] IS -0.5f and samples[2] IS 2.0f and samples[3] IS 1.0f);
   AUDIO_CHECK(idle.Processor.gain_reduction() IS 0 and idle.Processor.latency() IS 0);
   AUDIO_CHECK(not idle.Processor.pending() and idle.Processor.tail_frames() IS 0);

   // Targets hold DSP-ready values derived for the rate.
   LimiterTarget target;
   AUDIO_CHECK(not limiter_target(LimiterSettings(), 7999, target));
   AUDIO_REQUIRE(limiter_target(LimiterSettings(), 48000, target));
   AUDIO_CHECK(target.Rate IS 48000 and target.Gain IS 1);
   AUDIO_CHECK(target.Ceiling <= db_to_gain(-1) and db_to_gain(-1) - target.Ceiling < 1e-7);
   AUDIO_CHECK(double(float(target.Ceiling)) IS target.Ceiling);
   AUDIO_CHECK(std::abs(target.Release - std::exp(-1.0 / 4800.0)) < 1e-15);
   AUDIO_REQUIRE(limiter_target(LimiterSettings { .Ceiling = 0, .Release = 5, .Gain = -24 }, 8000, target));
   AUDIO_CHECK(target.Ceiling IS 1.0 and std::abs(target.Gain - 0.0630957344480193) < 1e-12);
   AUDIO_CHECK(std::abs(target.Release - std::exp(-1.0 / 40.0)) < 1e-15);
}

//********************************************************************************************************************
// An impulse emerges exactly one lookahead later, unchanged below the ceiling, whatever the block size.

static void test_latency(AudioTestContext &Test)
{
   for (auto [rate, expected] : { std::pair { 44100, 221 }, std::pair { 48000, 240 }, std::pair { 96000, 480 } }) {
      for (int channels = 1; channels <= 2; channels++) {
         for (int block : { 1, 7, 100000 }) {
            LimiterFixture fixture(rate, channels IS 2, LimiterSettings());
            AUDIO_CHECK(fixture.Processor.latency() IS expected);
            std::vector<float> buffer(size_t(expected) * 3 * channels, 0.0f);
            buffer[0] = 0.5f;
            if (channels IS 2) buffer[1] = -0.25f;
            const int frames = expected * 3;
            for (int position = 0; position < frames; position += block) {
               fixture.Processor.process(buffer.data() + size_t(position) * channels,
                  std::min(block, frames - position));
            }

            bool silent = true;
            for (int i = 0; i < frames * channels; i++) {
               if ((i / channels) != expected) silent &= buffer[i] IS 0;
            }
            AUDIO_CHECK(silent);
            AUDIO_CHECK(buffer[size_t(expected) * channels] IS 0.5f);
            if (channels IS 2) AUDIO_CHECK(buffer[size_t(expected) * 2 + 1] IS -0.25f);
            AUDIO_CHECK(fixture.Processor.gain_reduction() IS 0);
         }
      }
   }

   // Audio below the ceiling is delayed but otherwise bit-identical.
   uint32_t seed = 9;
   std::vector<float> input(4800 * 2);
   for (auto &sample : input) sample = float(noise(seed) * 0.85);
   LimiterFixture fixture(48000, true, LimiterSettings());
   auto output = input;
   fixture.process(output);
   bool identical = true;
   for (size_t i = 240 * 2; i < output.size(); i++) identical &= output[i] IS input[i - 240 * 2];
   AUDIO_CHECK(identical);
}

//********************************************************************************************************************
// Every output sample is at or below the represented ceiling for impulses, alternating peaks, square waves and noise
// with headroom up to magnitude 16, at every parameter limit, in mono and stereo.

static void test_ceiling(AudioTestContext &Test)
{
   auto make = [](int Kind, int Frames, int Channels, uint32_t &Seed) {
      std::vector<float> buffer(size_t(Frames) * Channels, 0.0f);
      for (int i = 0; i < Frames; i++) {
         for (int c = 0; c < Channels; c++) {
            double value = 0;
            switch (Kind) {
               case 0: value = noise(Seed) * 16.0; break;
               case 1: if ((i % 97) IS c * 13) value = ((i / 97) & 1) ? -16.0 : 15.5; break;
               case 2: value = ((i / 50) & 1) ? -4.0 : 4.0; break;
               default: value = (i & 1) ? -(1.0 + (i % 23) * 0.6) : (0.2 + (i % 11) * 1.4); break;
            }
            buffer[size_t(i) * Channels + c] = float(c ? value * 0.5 : value);
         }
      }
      return buffer;
   };

   bool bounded = true, exact = true, reduced = true;
   uint32_t seed = 31;
   for (int rate : { 8000, 48000, 192000 }) {
      for (int channels = 1; channels <= 2; channels++) {
         for (double ceiling : { -24.0, -1.0, 0.0 }) {
            for (double gain : { -24.0, 0.0, 24.0 }) {
               for (double release : { 5.0, 1000.0 }) {
                  for (int kind = 0; kind < 4; kind++) {
                     LimiterFixture fixture(rate, channels IS 2, LimiterSettings { ceiling, release, gain });
                     auto buffer = make(kind, 3000, channels, seed);
                     fixture.process(buffer);
                     const double peak = peak_of(buffer);
                     bounded &= peak <= float_ceiling(ceiling);
                     exact &= peak <= db_to_gain(ceiling);
                     if (gain > 0) reduced &= fixture.Processor.gain_reduction() > 0;
                  }
               }
            }
         }
      }
   }
   AUDIO_CHECK(bounded);
   AUDIO_CHECK(exact);
   AUDIO_CHECK(reduced);

   // A sustained peak at exactly the ceiling level is reproduced exactly, so the guard is not over-conservative.
   LimiterFixture fixture(48000, false, LimiterSettings { .Ceiling = 0, .Release = 100, .Gain = 0 });
   auto unity = square(1.0, 1000, 1);
   fixture.process(unity);
   AUDIO_CHECK(unity[240] IS 1.0f and unity[241] IS -1.0f and fixture.Processor.gain_reduction() IS 0);

   // A sustained over is held at the ceiling to within float rounding.
   LimiterFixture over(48000, false, LimiterSettings { .Ceiling = -6, .Release = 100, .Gain = 0 });
   auto loud = square(3.0, 4800, 1);
   over.process(loud);
   const double held = std::abs(double(loud.back()));
   AUDIO_CHECK(held <= float_ceiling(-6) and float_ceiling(-6) - held < 1e-6);
}

//********************************************************************************************************************
// Reduction is in place before a transient leaves the delay, holds while the peak can still affect the output and
// then recovers to 1/e of its dB value after one release time.

static void test_release(AudioTestContext &Test)
{
   for (int rate : { 44100, 48000 }) {
      const int lookahead = limiter_lookahead(rate);
      const int release_frames = rate / 10;
      LimiterFixture fixture(rate, false, LimiterSettings { .Ceiling = 0, .Release = 100, .Gain = 0 });
      const int spike = 1000;
      const int frames = spike + lookahead + 1 + release_frames;
      std::vector<float> input(frames, 0.125f);
      input[spike] = 2.0f;
      auto output = input;

      // Stop after the frame where one release time has elapsed since the peak left the window.
      fixture.process(output);
      const double required = 20.0 * std::log10(2.0);

      bool before = true, held = true;
      for (int n = lookahead; n < spike; n++) before &= output[n] IS 0.125f;
      for (int n = spike; n <= spike + lookahead; n++) {
         const double gain = double(output[n]) / double(input[n - lookahead]);
         held &= std::abs(gain - 0.5) < 1e-6;
      }
      AUDIO_CHECK(before and held);
      AUDIO_CHECK(std::abs(output[spike + lookahead]) <= 1.0f);
      AUDIO_CHECK(std::abs(fixture.Processor.gain_reduction() - required) < 1e-9);

      // The window loses the spike at spike + lookahead + 1, from which the envelope decays by one coefficient per
      // frame.
      const double expected = required * std::pow(std::exp(-1.0 / (0.1 * rate)), release_frames);
      AUDIO_CHECK(std::abs(fixture.Processor.envelope() - expected) < 1e-9);
      AUDIO_CHECK(std::abs(fixture.Processor.envelope() / required - std::exp(-1.0)) < 1e-9);

      const double first = double(output[spike + lookahead + 1]) / double(input[spike + 1]);
      AUDIO_CHECK(first > 0.5 and std::abs(-20.0 * std::log10(first) - required * std::exp(-1.0 / (0.1 * rate))) <
         1e-6);

      // The releasing envelope eventually snaps to exactly zero.
      std::vector<float> quiet(size_t(rate) * 5, 0.125f);
      fixture.process(quiet);
      AUDIO_CHECK(fixture.Processor.envelope() IS 0 and quiet.back() IS 0.125f);
   }
}

//********************************************************************************************************************
// Input gain is applied before detection and ramps over 10 ms without violating the ceiling.

static void test_input_gain(AudioTestContext &Test)
{
   for (double gain : { -24.0, 0.0, 24.0 }) {
      LimiterFixture fixture(48000, true, LimiterSettings { .Ceiling = 0, .Release = 100, .Gain = gain });
      auto buffer = square(0.01, 2400, 2);
      fixture.process(buffer);
      const double expected = 0.01 * db_to_gain(gain);
      AUDIO_CHECK(std::abs(std::abs(buffer.back()) - expected) < expected * 1e-6);
      AUDIO_CHECK(fixture.Processor.gain_reduction() IS 0);
   }

   // +24 dB on a loud signal is limited, and the reported reduction excludes the input gain.
   LimiterFixture loud(48000, false, LimiterSettings { .Ceiling = -1, .Release = 100, .Gain = 24 });
   auto buffer = square(0.5, 4800, 1);
   loud.process(buffer);
   AUDIO_CHECK(peak_of(buffer) <= float_ceiling(-1));
   AUDIO_CHECK(std::abs(loud.Processor.gain_reduction() - (24 + 20.0 * std::log10(0.5) + 1)) < 1e-5);

   // A ramp from 0 to +12 dB on a steady signal changes the applied gain gradually and arrives exactly.
   LimiterFixture ramp(48000, false, LimiterSettings { .Ceiling = 0, .Release = 100, .Gain = 0 });
   const int frames = 4800;
   const auto input = square(0.05, frames, 1);
   auto output = input;
   ramp.Processor.process(output.data(), 1000);
   ramp.change(LimiterSettings { .Ceiling = 0, .Release = 100, .Gain = 12 });
   ramp.Processor.process(output.data() + 1000, frames - 1000);

   const double target = db_to_gain(12);
   double largest = 0;
   bool monotonic = true;
   for (int n = 1240 + 1; n < frames; n++) {
      const double now = double(output[n]) / double(input[n - 240]);
      const double previous = double(output[n - 1]) / double(input[n - 241]);
      largest = std::max(largest, std::abs(now - previous));
      monotonic &= now >= previous - 1e-6;
   }
   AUDIO_CHECK(monotonic);
   AUDIO_CHECK(largest < (target - 1.0) / 480.0 + 1e-6);
   AUDIO_CHECK(std::abs(double(output[1240 + 480]) / double(input[1000 + 480]) - target) < 1e-6);
   AUDIO_CHECK(ramp.Processor.input_gain() IS target);

   // Raising the gain while loud audio is buffered and entering cannot violate the ceiling.
   for (double jump : { 24.0, -24.0 }) {
      LimiterFixture fixture(48000, true, LimiterSettings { .Ceiling = -1, .Release = 5, .Gain = -jump });
      uint32_t seed = 41;
      std::vector<float> noisy(9600 * 2);
      for (auto &sample : noisy) sample = float(noise(seed) * 8.0);
      fixture.Processor.process(noisy.data(), 3000);
      fixture.change(LimiterSettings { .Ceiling = -1, .Release = 5, .Gain = jump });
      fixture.Processor.process(noisy.data() + 3000 * 2, 6600);
      AUDIO_CHECK(peak_of(noisy) <= float_ceiling(-1));
   }
}

//********************************************************************************************************************
// A lower ceiling applies to the first output after the edit, including buffered peaks.  A higher ceiling recovers
// through release rather than jumping.

static void test_ceiling_edits(AudioTestContext &Test)
{
   for (int channels = 1; channels <= 2; channels++) {
      LimiterFixture fixture(48000, channels IS 2, LimiterSettings { .Ceiling = 0, .Release = 100, .Gain = 0 });
      auto buffer = square(0.95, 4800, channels);
      fixture.Processor.process(buffer.data(), 1000);
      AUDIO_CHECK(fixture.Processor.gain_reduction() IS 0);
      fixture.change(LimiterSettings { .Ceiling = -24, .Release = 100, .Gain = 0 });
      fixture.Processor.process(buffer.data() + 1000 * channels, 1);
      AUDIO_CHECK(std::abs(buffer[1000 * channels]) <= float_ceiling(-24));
      fixture.Processor.process(buffer.data() + 1001 * channels, 4800 - 1001);
      AUDIO_CHECK(peak_of(buffer, 1000 * channels) <= float_ceiling(-24));
      AUDIO_CHECK(fixture.Processor.gain_reduction() > 23);

      // Raise the ceiling again.  The envelope releases by exactly one coefficient on the next frame.
      const double envelope = fixture.Processor.envelope();
      const double before = std::abs(double(buffer.back()));
      fixture.change(LimiterSettings { .Ceiling = 0, .Release = 100, .Gain = 0 });
      auto next = square(0.95, 4800, channels);
      fixture.Processor.process(next.data(), 1);
      AUDIO_CHECK(std::abs(fixture.Processor.envelope() - envelope * std::exp(-1.0 / 4800.0)) < 1e-9);
      const double after = std::abs(double(next[0]));
      AUDIO_CHECK(after > before and after < before * db_to_gain(0.01));

      fixture.Processor.process(next.data() + channels, 4799);
      double largest = 0;
      for (int n = 1; n < 4800; n++) {
         const double step = std::abs(double(next[n * channels])) / std::abs(double(next[(n - 1) * channels]));
         largest = std::max(largest, step);
      }
      AUDIO_CHECK(largest < db_to_gain(0.01));
   }
}

//********************************************************************************************************************
// One loud channel controls both gains, and channel swaps or polarity inversions do not alter the trajectory.

static void test_linking(AudioTestContext &Test)
{
   const int frames = 9600, lookahead = 240;
   auto make = [&](bool Swap, double LoudSign) {
      std::vector<float> buffer(frames * 2);
      for (int i = 0; i < frames; i++) {
         const double loud = LoudSign * 2.0 * std::sin(2.0 * std::numbers::pi * 110.0 * i / 48000.0);
         const double quiet = 0.01 * std::sin(2.0 * std::numbers::pi * 1000.0 * i / 48000.0 + 0.3);
         buffer[i * 2] = float(Swap ? quiet : loud);
         buffer[i * 2 + 1] = float(Swap ? loud : quiet);
      }
      return buffer;
   };

   auto gains = [&](bool Swap, double LoudSign, bool &Linked) {
      LimiterFixture fixture(48000, true, LimiterSettings());
      const auto input = make(Swap, LoudSign);
      auto output = input;
      fixture.process(output);
      std::vector<double> result(frames, 1.0);
      for (int i = lookahead; i < frames; i++) {
         const int source = i - lookahead;
         if ((input[source * 2] != 0) and (input[source * 2 + 1] != 0)) {
            const double left = double(output[i * 2]) / double(input[source * 2]);
            const double right = double(output[i * 2 + 1]) / double(input[source * 2 + 1]);
            Linked &= std::abs(left - right) < 1e-5;
            result[i] = left;
         }
      }
      return std::pair { result, output };
   };

   bool linked = true;
   const auto [base, base_output] = gains(false, 1, linked);
   const auto [swapped, swapped_output] = gains(true, 1, linked);
   const auto [inverted, inverted_output] = gains(false, -1, linked);
   AUDIO_CHECK(linked);
   bool same = true, limited = false, mirrored = true;
   for (int i = 0; i < frames; i++) {
      same &= (std::abs(base[i] - swapped[i]) < 1e-6) and (std::abs(base[i] - inverted[i]) < 1e-6);
      limited |= base[i] < 0.5;
      mirrored &= (base_output[i * 2] IS swapped_output[i * 2 + 1]) and
         (base_output[i * 2 + 1] IS swapped_output[i * 2]);
      mirrored &= (base_output[i * 2] IS -inverted_output[i * 2]) and
         (base_output[i * 2 + 1] IS inverted_output[i * 2 + 1]);
   }
   AUDIO_CHECK(same and limited and mirrored);
   AUDIO_CHECK(peak_of(base_output) <= float_ceiling(-1));

   // A silent right channel leaves the left identical to mono processing.
   LimiterFixture stereo(48000, true, LimiterSettings()), mono(48000, false, LimiterSettings());
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
// Block boundaries do not change the output, the final state or the maximum reduction, including edits during the
// 10 ms ramp and during release.

static void test_blocks(AudioTestContext &Test)
{
   uint32_t seed = 3;
   std::vector<float> input(19200 * 2);
   for (int i = 0; i < 19200; i++) {
      const double envelope = (i / 2400) & 1 ? 3.0 : 0.05;
      input[i * 2] = float(noise(seed) * envelope);
      input[i * 2 + 1] = float(noise(seed) * envelope);
   }

   const LimiterSettings edits[4] = {
      { .Ceiling = -6, .Release = 20, .Gain = 12 },
      { .Ceiling = -12, .Release = 500, .Gain = -6 },  // During the first ramp
      { .Ceiling = -0.5, .Release = 5, .Gain = 3 },    // Rapid replacement of the endpoint
      { .Ceiling = -3, .Release = 1000, .Gain = 0 }    // During release of a loud burst
   };

   auto render = [&](int Pattern, std::array<double, 5> &Maximum, double &Envelope) {
      static const int irregular[] = { 1, 7, 64, 3, 129, 480, 2 };
      LimiterFixture fixture(48000, true, LimiterSettings());
      auto buffer = input;
      int position = 0, cycle = 0;
      const int stops[] = { 4800, 5000, 5100, 7300, 19200 };
      for (int stage = 0; stage < 5; stage++) {
         Maximum[stage] = 0;
         while (position < stops[stage]) {
            int block = stops[stage] - position;
            if (Pattern IS 1) block = 1;
            else if (Pattern IS 2) block = std::min(block, irregular[cycle++ % std::size(irregular)]);
            fixture.Processor.process(buffer.data() + position * 2, block);
            Maximum[stage] = std::max(Maximum[stage], fixture.Processor.gain_reduction());
            position += block;
         }
         if (stage < 4) fixture.change(edits[stage]);
      }
      Envelope = fixture.Processor.envelope();
      return buffer;
   };

   std::array<double, 5> whole_max, single_max, irregular_max;
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
   AUDIO_CHECK(whole_max[4] > 5);
}

//********************************************************************************************************************
// Pending output drains through the delay without a tail; reset discards buffered audio and transition state.

static void test_drain_and_reset(AudioTestContext &Test)
{
   for (int channels = 1; channels <= 2; channels++) {
      LimiterFixture fixture(48000, channels IS 2, LimiterSettings { .Ceiling = -1, .Release = 1000, .Gain = 0 });
      AUDIO_CHECK(fixture.Processor.tail() IS AudioTail::NONE);
      AUDIO_CHECK(fixture.Processor.tail_frames() IS 240 and fixture.Processor.latency() IS 240);
      AUDIO_CHECK(fixture.Processor.decay_estimate() IS 0);
      AUDIO_CHECK(not fixture.Processor.pending());

      // 100 frames of loud input, the last of which is non-zero, followed by silence one frame at a time.
      auto buffer = square(2.0, 100, channels);
      fixture.process(buffer);
      AUDIO_CHECK(fixture.Processor.pending());
      int last = -1, cleared = -1;
      std::vector<float> frame(channels);
      for (int n = 100; n < 1000; n++) {
         std::fill(frame.begin(), frame.end(), 0.0f);
         fixture.Processor.process(frame.data(), 1);
         if (frame[0] != 0) last = n;
         if ((cleared < 0) and (not fixture.Processor.pending())) cleared = n;
      }
      AUDIO_CHECK(last IS 99 + 240);
      AUDIO_CHECK(cleared IS 99 + 240);

      // The release envelope alone does not keep the processor pending.
      AUDIO_CHECK(fixture.Processor.envelope() > 1 and not fixture.Processor.pending());

      // Reset discards buffered audio, the envelope and an unfinished ramp, then adopts the latest settings.
      auto loud = square(4.0, 120, channels);
      fixture.process(loud);
      const LimiterSettings edited { .Ceiling = -6, .Release = 50, .Gain = 6 };
      fixture.change(edited);
      auto partial = square(4.0, 100, channels);
      fixture.process(partial);
      AUDIO_CHECK(fixture.Processor.pending());
      fixture.Processor.reset();
      AUDIO_CHECK(not fixture.Processor.pending());
      AUDIO_CHECK(fixture.Processor.envelope() IS 0 and fixture.Processor.gain_reduction() IS 0);
      AUDIO_CHECK(fixture.Processor.input_gain() IS db_to_gain(6));

      LimiterFixture fresh(48000, channels IS 2, edited);
      uint32_t seed = 5;
      std::vector<float> a(4800 * channels);
      for (auto &sample : a) sample = float(noise(seed) * 1.5);
      auto b = a;
      fixture.process(a);
      fresh.process(b);
      AUDIO_CHECK(a IS b);
      bool silent = true;
      for (int i = 0; i < 240 * channels; i++) silent &= a[i] IS 0;
      AUDIO_CHECK(silent);
   }

   // Edits while the output is unconfigured are adopted by the next configuration.
   const LimiterSettings edited { .Ceiling = -9, .Release = 30, .Gain = 4 };
   LimiterFixture unconfigured(0, false, LimiterSettings());
   unconfigured.change(edited);
   unconfigured.Effect.OutputRate = 48000;
   std::unique_ptr<AudioEffectConfiguration> late;
   AUDIO_REQUIRE(unconfigured.Processor.prepare(48000, false, late) IS ERR::Okay);
   late->publish();
   unconfigured.Processor.reset();
   LimiterFixture mono(48000, false, edited);
   auto c = square(0.5, 4800, 1), d = c;
   unconfigured.process(c);
   mono.process(d);
   AUDIO_CHECK(c IS d);
}

//********************************************************************************************************************
// gain_reduction() reports the maximum of the most recent call, reads are non-destructive, and every call starts from
// zero.

static void test_reduction_meter(AudioTestContext &Test)
{
   LimiterFixture fixture(48000, false, LimiterSettings { .Ceiling = 0, .Release = 50, .Gain = 0 });
   auto buffer = square(2.0, 4800, 1);
   buffer.resize(9600, 0.0f);
   fixture.process(buffer);
   const double reduction = fixture.Processor.gain_reduction();
   AUDIO_CHECK(std::abs(reduction - 20.0 * std::log10(2.0)) < 1e-9);
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
// Integration with extAudioEffect: meter layout, latency, interval aggregation, bypass, idle and serial chains.

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
   ChainFixture mono(44100, 1);
   AUDIO_REQUIRE(mono.add(std::make_unique<LimiterProcessor>(&mono.Effects[0], LimiterSettings()),
      &glLimiterSchema) IS ERR::Okay);
   auto &mono_effect = mono.Effects[0];
   AUDIO_REQUIRE(mono_effect.Meters.size() IS 3);
   AUDIO_CHECK(std::string_view(mono_effect.Meters[0].Key) IS "input_peak_centre");
   AUDIO_CHECK(std::string_view(mono_effect.Meters[1].Key) IS "output_peak_centre");
   AUDIO_CHECK(std::string_view(mono_effect.Meters[2].Key) IS "gain_reduction");
   AUDIO_CHECK(mono_effect.latency() IS 221);

   ChainFixture fixture(48000, 2);
   auto owned = std::make_unique<LimiterProcessor>(&fixture.Effects[0], LimiterSettings());
   auto limiter = owned.get();
   AUDIO_REQUIRE(fixture.add(std::move(owned), &glLimiterSchema) IS ERR::Okay);
   auto &effect = fixture.Effects[0];
   AUDIO_CHECK(effect.latency() IS 240);
   AUDIO_REQUIRE(effect.Meters.size() IS 5);
   const char *keys[] = { "input_peak_left", "input_peak_right", "output_peak_left", "output_peak_right",
      "gain_reduction" };
   for (int i = 0; i < 5; i++) AUDIO_CHECK(std::string_view(effect.Meters[i].Key) IS keys[i]);
   AUDIO_CHECK(std::string_view(effect.Meters[4].Scope) IS "global");
   AUDIO_CHECK(std::string_view(effect.Meters[4].Semantics) IS "maximum-attenuation");
   AUDIO_CHECK(effect.Meters[4].Channel IS 0 and std::string_view(effect.Meters[4].Unit) IS "dB");

   const auto xml = build_schema_xml(glLimiterSchema, effect.Meters);
   AUDIO_CHECK(xml.find("<effect class=\"AudioLimiter\" version=\"1\"") != std::string::npos);
   AUDIO_CHECK(xml.find("key=\"gain_reduction\" type=\"scalar\"") != std::string::npos);
   AUDIO_CHECK(xml.find("slot=\"4\"") != std::string::npos);

   // One 50 ms interval of a loud left channel, processed in uneven blocks.  Input peaks precede the limiter and the
   // output is held at the ceiling.
   auto loud = square(2.0, 2400, 2);
   for (int i = 0; i < 2400; i++) loud[i * 2 + 1] *= 0.25f;
   int position = 0;
   for (int block : { 7, 1000, 393, 1000 }) {
      effect.process(loud.data() + position * 2, block);
      position += block;
   }
   AUDIO_CHECK(effect.Meter.Sequence IS 1 and effect.Meter.Interval IS 2400);
   AUDIO_CHECK((effect.Meter.Flags & AMF::VALID) != AMF::NIL);
   AUDIO_CHECK(std::abs(effect.Meter.Values[0] - 20.0 * std::log10(2.0)) < 1e-6);
   AUDIO_CHECK(std::abs(effect.Meter.Values[1] - 20.0 * std::log10(0.5)) < 1e-6);
   AUDIO_CHECK(effect.Meter.Values[2] <= -1 + 1e-6 and effect.Meter.Values[2] > -1.001);
   AUDIO_CHECK(std::abs(effect.Meter.Values[3] - (effect.Meter.Values[2] + 20.0 * std::log10(0.25))) < 1e-5);
   AUDIO_CHECK(std::abs(effect.Meter.Values[4] - (20.0 * std::log10(2.0) + 1)) < 1e-5);
   AUDIO_CHECK(effect.Meter.ValueFlags[4] IS int(AMV::VALID));
   AUDIO_CHECK(effects_pending(*fixture.Chain));

   // During release the interval reports its greatest reduction, not the final value.
   std::vector<float> silence(2400 * 2, 0.0f);
   effect.process(silence.data(), 2400);
   AUDIO_CHECK(effect.Meter.Sequence IS 2);
   AUDIO_CHECK(effect.Meter.Values[4] > 7 and limiter->envelope() < 7);
   AUDIO_CHECK(not effects_pending(*fixture.Chain));

   // Bypass leaves the buffer untouched and reports no latency; leaving bypass discards buffered audio.
   auto again = square(2.0, 1200, 2);
   effect.process(again.data(), 1200);
   AUDIO_CHECK(limiter->pending());
   effect.Channel = 1 << 16;
   AUDIO_CHECK(AUDIOEFFECT_SET_Flags(&effect, AEF::BYPASS) IS ERR::Okay);
   AUDIO_CHECK(effect.latency() IS 0 and not effects_pending(*fixture.Chain));
   int64_t chain_latency = -1;
   AUDIO_CHECK(fixture.Chain->latency(chain_latency) IS ERR::Okay and chain_latency IS 0);
   auto bypassed = square(2.0, 480, 2), original = bypassed;
   effect.process(bypassed.data(), 480);
   AUDIO_CHECK(bypassed IS original);
   AUDIO_CHECK(AUDIOEFFECT_SET_Flags(&effect, AEF::NIL) IS ERR::Okay);
   AUDIO_CHECK(effect.latency() IS 240);
   std::array<float, 2> frame = { 0.5f, 0.5f };
   effect.process(frame.data(), 1);
   AUDIO_CHECK(limiter->envelope() IS 0 and frame[0] IS 0 and frame[1] IS 0);

   // Idle discards buffered audio and the envelope.
   again = square(2.0, 1200, 2);
   effect.process(again.data(), 1200);
   AUDIO_CHECK(limiter->envelope() > 6 and limiter->pending());
   effect.idle();
   AUDIO_CHECK(limiter->envelope() IS 0 and limiter->gain_reduction() IS 0 and not limiter->pending());
   AUDIO_CHECK((effect.Meter.Flags & AMF::IDLE) != AMF::NIL);
   AUDIO_CHECK(not effects_pending(*fixture.Chain));
}

//********************************************************************************************************************
// Compressor -> limiter: serial latency is the limiter's, and the buffered output drains after the source stops
// without truncation.

static void test_chains(AudioTestContext &Test)
{
   ChainFixture chain(48000, 2);
   AUDIO_REQUIRE(chain.add(std::make_unique<CompressorProcessor>(&chain.Effects[0], CompressorSettings())) IS
      ERR::Okay);
   AUDIO_REQUIRE(chain.add(std::make_unique<LimiterProcessor>(&chain.Effects[1], LimiterSettings()),
      &glLimiterSchema) IS ERR::Okay);

   int64_t latency = 0;
   AUDIO_CHECK(chain.Chain->latency(latency) IS ERR::Okay and latency IS 240);
   AUDIO_CHECK(chain.Chain->tail_bound() IS 240 and chain.Chain->decay_estimate() IS 240);

   uint32_t seed = 23;
   std::vector<float> source(4800 * 2);
   for (auto &sample : source) sample = float(noise(seed) * 2.0);
   render_effects(*chain.Chain, source.data(), 4800, 4800, 48000, true);
   AUDIO_CHECK(chain.Chain->State IS ADS::ACTIVE);
   AUDIO_CHECK(peak_of(source) <= float_ceiling(-1));

   // The first drain period carries the final 240 buffered frames and then becomes idle.
   std::vector<float> drain(480 * 2, 0.0f);
   render_effects(*chain.Chain, drain.data(), 480, 0, 48000);
   bool tail = true, silent = true;
   for (int i = 0; i < 240 * 2; i++) tail &= drain[i] != 0;
   for (int i = 240 * 2; i < 480 * 2; i++) silent &= drain[i] IS 0;
   AUDIO_CHECK(tail and silent);
   AUDIO_CHECK(chain.Chain->State IS ADS::IDLE and not chain.Chain->Truncated);

   // Limiter -> limiter latencies are additive.
   ChainFixture serial(96000, 1);
   AUDIO_REQUIRE(serial.add(std::make_unique<LimiterProcessor>(&serial.Effects[0], LimiterSettings())) IS
      ERR::Okay);
   AUDIO_REQUIRE(serial.add(std::make_unique<LimiterProcessor>(&serial.Effects[1], LimiterSettings())) IS
      ERR::Okay);
   AUDIO_CHECK(serial.Chain->latency(latency) IS ERR::Okay and latency IS 960);
   std::vector<float> impulse(2000, 0.0f);
   impulse[0] = 0.5f;
   render_effects(*serial.Chain, impulse.data(), 2000, 1, 96000, true);
   AUDIO_CHECK(impulse[960] IS 0.5f and peak_of(impulse) IS 0.5);
}

//********************************************************************************************************************
// Outputs and state remain finite at every parameter limit, and non-finite input is treated as silence.

static void test_robustness(AudioTestContext &Test)
{
   bool finite = true, bounded = true;
   uint32_t seed = 13;
   for (int rate : { 8000, 192000 }) {
      for (int combination = 0; combination < 8; combination++) {
         auto pick = [&](int Bit, double Low, double High) { return (combination & (1 << Bit)) ? High : Low; };
         const LimiterSettings settings { .Ceiling = pick(0, -24, 0), .Release = pick(1, 5, 1000),
            .Gain = pick(2, -24, 24) };
         LimiterFixture fixture(rate, true, settings);
         std::vector<float> buffer(4000 * 2);
         for (auto &sample : buffer) sample = float(noise(seed) * 16.0);
         buffer[10] = std::numeric_limits<float>::max();
         buffer[11] = -std::numeric_limits<float>::max();
         fixture.process(buffer);
         for (auto sample : buffer) finite &= std::isfinite(sample);
         bounded &= peak_of(buffer) <= float_ceiling(settings.Ceiling);
         finite &= std::isfinite(fixture.Processor.envelope()) and std::isfinite(fixture.Processor.gain_reduction());
      }
   }
   AUDIO_CHECK(finite and bounded);

   // NaN and infinities are silenced on entry and do not disturb the surrounding audio.
   LimiterFixture poisoned(48000, true, LimiterSettings()), clean(48000, true, LimiterSettings());
   auto a = square(0.5, 2400, 2), b = a;
   a[200] = std::numeric_limits<float>::quiet_NaN();
   a[201] = std::numeric_limits<float>::infinity();
   a[1001] = -std::numeric_limits<float>::infinity();
   b[200] = b[201] = b[1001] = 0;
   poisoned.process(a);
   clean.process(b);
   AUDIO_CHECK(a IS b);
   AUDIO_CHECK(std::isfinite(poisoned.Processor.envelope()) and poisoned.Processor.gain_reduction() IS 0);
}

//********************************************************************************************************************
// Throughput is recorded for comparison between builds; it is not a pass/fail criterion.  Storage must not move
// during processing, which would indicate an allocation on the render thread.

static void report_throughput(AudioTestContext &Test)
{
   for (int rate : { 48000, 192000 }) {
      LimiterFixture fixture(rate, true, LimiterSettings { .Ceiling = -1, .Release = 100, .Gain = 12 });
      const double *storage = fixture.Processor.delay_storage();
      uint32_t seed = 1;
      std::vector<float> buffer(size_t(rate) * 2);
      for (auto &sample : buffer) sample = float(noise(seed) * 0.8);
      const auto start = std::chrono::steady_clock::now();
      for (int second = 0; second < 5; second++) fixture.process(buffer);
      const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      AUDIO_CHECK(fixture.Processor.delay_storage() IS storage);
      kt::Log("AudioTests").msg("Limiter stereo: %.0fx real-time at %d Hz.", 5.0 / elapsed, rate);
   }
}

//********************************************************************************************************************

static void run(AudioTestContext &Test)
{
   test_configuration(Test);
   test_latency(Test);
   test_ceiling(Test);
   test_release(Test);
   test_input_gain(Test);
   test_ceiling_edits(Test);
   test_linking(Test);
   test_blocks(Test);
   test_drain_and_reset(Test);
   test_reduction_meter(Test);
   test_effect_integration(Test);
   test_chains(Test);
   test_robustness(Test);
   report_throughput(Test);
}

} // namespace audio_tests_audio_limiter_dsp
