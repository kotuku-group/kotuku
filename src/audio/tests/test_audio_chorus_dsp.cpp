// Included by audio.cpp to exercise the module implementation.

#include <chrono>

namespace audio_tests_audio_chorus_dsp {

// Configure exactly as activation does: prepare outside the lock, publish, then reset on the render thread.

struct ChorusFixture {
   extAudioEffect Effect{nullptr, 1};
   ChorusProcessor Processor;
   ERR Prepared;
   int Channels;

   ChorusFixture(int Rate, bool Stereo, const ChorusSettings &Settings) : Processor(&Effect, Settings) {
      Effect.OutputRate = Rate;
      Effect.Stereo = Stereo;
      Effect.ResetPending = false;
      Channels = Stereo ? 2 : 1;
      std::unique_ptr<AudioEffectConfiguration> config;
      Prepared = Processor.prepare(Rate, Stereo, config);
      if (config) config->publish();
      Processor.reset();
   }

   // Parameter updates derive their target on the control thread, as ChorusUpdate does.
   void change(const ChorusSettings &Settings) {
      ChorusTarget target;
      const bool valid = chorus_target(Settings, Processor.SampleRate, target);
      Processor.update(Settings, valid ? &target : nullptr);
   }

   void run(std::vector<float> &Buffer, int Block = 0) {
      const int frames = int(Buffer.size()) / Channels;
      if (Block <= 0) Block = frames;
      for (int i = 0; i < frames; i += Block) {
         Processor.process(Buffer.data() + size_t(i) * Channels, std::min(Block, frames - i));
      }
   }

   std::vector<float> impulse(int Frames, float Left = 1.0f, float Right = 0.0f) {
      std::vector<float> buffer(size_t(Frames) * Channels, 0.0f);
      buffer[0] = Left;
      if (Channels IS 2) buffer[1] = Right;
      run(buffer);
      return buffer;
   }
};

static ChorusSettings wet_settings(double Rate, double Delay, double Depth, double Spread = 50)
{
   return ChorusSettings { .Rate = Rate, .Delay = Delay, .Depth = Depth, .Spread = Spread, .Mix = 100 };
}

static double noise(uint32_t &Seed)
{
   Seed = Seed * 1664525u + 1013904223u;
   return double(Seed >> 8) / double(1u << 23) - 1.0;
}

static std::vector<float> noise_buffer(int Frames, int Channels, uint32_t Seed, double Level = 0.5)
{
   std::vector<float> buffer(size_t(Frames) * Channels);
   for (auto &sample : buffer) sample = float(noise(Seed) * Level);
   return buffer;
}

//********************************************************************************************************************
// An independent model of the published equations with unbounded history.  The phase is computed from the frame
// number rather than accumulated, and the read position is not clamped.

static std::vector<double> reference(const std::vector<float> &Input, int Channels, const ChorusSettings &Settings,
   int Rate)
{
   const int frames = int(Input.size()) / Channels;
   const double centre = Settings.Delay * double(Rate) / 1000.0;
   const double depth = Settings.Depth * double(Rate) / 1000.0;
   const double m = Settings.Mix / 100.0;
   const double cycle = 2.0 * std::numbers::pi;

   auto past = [&](int Frame, int Channel) {
      if (Frame < 0) return 0.0;
      const float x = Input[size_t(Frame) * Channels + Channel];
      return std::isfinite(x) ? double(x) : 0.0;
   };

   std::vector<double> output(Input.size(), 0.0);
   for (int n = 0; n < frames; n++) {
      const double phi = std::fmod(double(n) * cycle * Settings.Rate / double(Rate), cycle);
      for (int c = 0; c < Channels; c++) {
         const double offset = (c IS 1) ? std::numbers::pi * Settings.Spread / 100.0 : 0.0;
         const double d = centre + depth * std::sin(phi + offset);
         const int whole = int(std::floor(d));
         const double fraction = d - double(whole);
         const double wet = past(n - whole, c) * (1.0 - fraction) + past(n - whole - 1, c) * fraction;
         output[size_t(n) * Channels + c] = (1.0 - m) * past(n, c) + m * wet;
      }
   }
   return output;
}

static double max_error(const std::vector<float> &Actual, const std::vector<double> &Expected)
{
   double error = 0;
   for (size_t i = 0; i < Actual.size(); i++) error = std::max(error, std::abs(double(Actual[i]) - Expected[i]));
   return error;
}

static double peak(const std::vector<float> &Buffer)
{
   double result = 0;
   for (auto sample : Buffer) result = std::max(result, double(std::abs(sample)));
   return result;
}

//********************************************************************************************************************

static void test_configuration(AudioTestContext &Test)
{
   ChorusFixture low(7999, false, ChorusSettings());
   AUDIO_CHECK(low.Prepared IS ERR::NoSupport);
   ChorusFixture high(192001, true, ChorusSettings());
   AUDIO_CHECK(high.Prepared IS ERR::NoSupport);

   // Supported endpoints allocate a ring covering 39 ms plus interpolation support.
   ChorusFixture slow(8000, false, ChorusSettings()), fast(192000, true, ChorusSettings());
   AUDIO_CHECK(slow.Prepared IS ERR::Okay and fast.Prepared IS ERR::Okay);
   AUDIO_CHECK(slow.Processor.ring_capacity() IS 314 and slow.Processor.ring_samples() IS 314);
   AUDIO_CHECK(fast.Processor.ring_capacity() IS 7490 and fast.Processor.ring_samples() IS 14980);
   AUDIO_CHECK(chorus_capacity(44100) IS 1722); // 1719.9 frames rounds up

   // An unconfigured processor passes audio through and reports no tail.
   ChorusFixture idle(0, true, ChorusSettings());
   AUDIO_CHECK(idle.Prepared IS ERR::Okay);
   float samples[] = { 0.25f, -0.5f, 0.75f, 1.0f };
   idle.Processor.process(samples, 2);
   AUDIO_CHECK(samples[0] IS 0.25f and samples[1] IS -0.5f and samples[2] IS 0.75f and samples[3] IS 1.0f);
   AUDIO_CHECK((not idle.Processor.pending()) and (idle.Processor.tail_frames() IS 0));
   AUDIO_CHECK(idle.Processor.decay_estimate() IS 0);

   // Zero algorithmic latency and a finite tail equal to the readable history at every setting.
   ChorusFixture typical(48000, true, ChorusSettings());
   AUDIO_CHECK(typical.Processor.latency() IS 0 and typical.Processor.tail() IS AudioTail::FINITE);
   AUDIO_CHECK(typical.Processor.tail_frames() IS 1874 and typical.Processor.decay_estimate() IS 1874);
   AUDIO_CHECK(typical.Processor.centre_delay() IS 720.0 and typical.Processor.depth_frames() IS 144.0);
   AUDIO_CHECK(std::abs(typical.Processor.phase_offset() - std::numbers::pi * 0.5) < 1e-15);
   AUDIO_CHECK(typical.Processor.wet_mix() IS 0.35);
   AUDIO_CHECK(typical.Processor.lfo_phase() IS 0);
}

//********************************************************************************************************************
// At zero depth the wet path is a static fractional delay: matches an independent reference, exact at integer
// delays, and independent of rate and spread.

static void test_static_delay(AudioTestContext &Test)
{
   // An integer delay reproduces the impulse exactly.
   {
      ChorusFixture fixture(48000, false, wet_settings(3, 10, 0));
      const auto output = fixture.impulse(1000);
      bool single = true;
      for (int i = 0; i < 1000; i++) if (i != 480) single &= output[i] IS 0;
      AUDIO_CHECK(single and output[480] IS 1.0f);
   }

   // A fractional delay spreads the impulse across two frames with linear weights that sum to one.
   {
      ChorusFixture fixture(8000, false, wet_settings(1, 10.03125, 0)); // 80.25 frames
      AUDIO_CHECK(fixture.Processor.centre_delay() IS 80.25);
      const auto output = fixture.impulse(200);
      AUDIO_CHECK(output[79] IS 0 and output[80] IS 0.75f and output[81] IS 0.25f and output[82] IS 0);
   }

   // Zero mix is the input, bit for bit, including headroom above unity; history and phase continue.
   for (int channels = 1; channels <= 2; channels++) {
      auto settings = wet_settings(10, 30, 9, 100);
      settings.Mix = 0;
      ChorusFixture fixture(48000, channels IS 2, settings);
      const auto input = noise_buffer(4801, channels, 7, 4.0);
      auto output = input;
      fixture.run(output, 17);
      bool identical = true;
      for (size_t i = 0; i < input.size(); i++) identical &= output[i] IS input[i];
      AUDIO_CHECK(identical and fixture.Processor.pending() and (fixture.Processor.lfo_phase() > 0));
   }

   // All-wet output removes the dry signal; a partial mix is the linear blend of the two.
   {
      ChorusFixture wet(48000, true, wet_settings(1, 20, 0));
      float frame[2] = { 0.5f, -0.25f };
      wet.Processor.process(frame, 1);
      AUDIO_CHECK(frame[0] IS 0 and frame[1] IS 0);

      auto settings = wet_settings(1, 20, 0);
      settings.Mix = 30;
      ChorusFixture partial(48000, false, settings);
      const auto output = partial.impulse(1000, 0.8f);
      AUDIO_CHECK(std::abs(output[0] - 0.56f) < 1e-7 and std::abs(output[960] - 0.24f) < 1e-7);
   }

   // Rate and spread have no effect at zero depth.
   const auto input = noise_buffer(9600, 2, 21);
   auto first = input, second = input;
   ChorusFixture a(48000, true, wet_settings(0.05, 13.37, 0, 0)), b(48000, true, wet_settings(10, 13.37, 0, 100));
   a.run(first, 256);
   b.run(second, 17);
   bool same = true;
   for (size_t i = 0; i < first.size(); i++) same &= first[i] IS second[i];
   AUDIO_CHECK(same);

   // Every supported rate, both layouts and several mixes against the independent reference.
   for (int rate : { 8000, 44100, 48000, 96000, 192000 }) {
      for (int channels = 1; channels <= 2; channels++) {
         for (double mix : { 0.0, 35.0, 100.0 }) {
            auto settings = wet_settings(2, 17.3, 0);
            settings.Mix = mix;
            ChorusFixture fixture(rate, channels IS 2, settings);
            auto buffer = noise_buffer(rate / 20, channels, 11);
            const auto expected = reference(buffer, channels, settings, rate);
            fixture.run(buffer, 256);
            AUDIO_CHECK(max_error(buffer, expected) < 1e-6);
         }
      }
   }
}

//********************************************************************************************************************
// The modulated read position matches the reference at the rate and depth extremes, all spreads and the delay
// range endpoints.  Mono ignores spread and stereo channels remain isolated.

static void test_modulation(AudioTestContext &Test)
{
   const ChorusSettings cases[] = {
      wet_settings(0.05, 15, 3, 50), wet_settings(10, 30, 9, 100), wet_settings(10, 10, 9, 0),
      wet_settings(7.5, 22.2, 4.4, 50), { .Rate = 3.3, .Delay = 11, .Depth = 8.5, .Spread = 25, .Mix = 35 }
   };
   for (int rate : { 8000, 44100, 48000, 96000, 192000 }) {
      for (int channels = 1; channels <= 2; channels++) {
         for (const auto &settings : cases) {
            ChorusFixture fixture(rate, channels IS 2, settings);
            auto buffer = noise_buffer(rate / 5, channels, 13);
            const auto expected = reference(buffer, channels, settings, rate);
            fixture.run(buffer, 256);
            AUDIO_CHECK(max_error(buffer, expected) < 1e-5);
         }
      }
   }

   // Known LFO positions, recovered from a sawtooth input whose linear interpolation is exact between wraps.  The
   // read distance at frame n is the sawtooth position minus the output.  Quarter cycles of the extreme settings
   // reach the 1 ms and 39 ms limits.
   struct Probe { int Rate; double Lfo, Delay, Depth, Spread; };
   const Probe probes[] = {
      { 48000, 10, 30, 9, 100 }, { 48000, 10, 10, 9, 50 }, { 8000, 0.05, 30, 9, 100 }, { 192000, 10, 10, 9, 0 }
   };
   const int period = 16384;
   const double step = 1.0 / double(period);
   for (const auto &probe : probes) {
      ChorusFixture fixture(probe.Rate, true, wet_settings(probe.Lfo, probe.Delay, probe.Depth, probe.Spread));
      const int quarter = int(std::lround(double(probe.Rate) / (4.0 * probe.Lfo)));
      const int frames = quarter * 11 + 3;
      std::vector<float> buffer(size_t(frames) * 2);
      for (int n = 0; n < frames; n++) {
         buffer[n * 2] = buffer[n * 2 + 1] = float(double((n % period) - period / 2) * step);
      }
      fixture.run(buffer, 1000);

      const double centre = probe.Delay * probe.Rate / 1000.0, depth = probe.Depth * probe.Rate / 1000.0;
      const double offset = std::numbers::pi * probe.Spread / 100.0;
      bool matched = true;
      int checked[4] = { 0, 0, 0, 0 };
      for (int cycle = 0; cycle <= 2; cycle++) {
         for (int k = 1; k <= 3; k++) {
            // The frames around each quarter cycle whose interpolation does not straddle a sawtooth wrap.
            const int middle = quarter * (cycle * 4 + k);
            for (int n = middle - 2; n <= middle + 2; n++) {
               const double phi = 2.0 * std::numbers::pi * probe.Lfo * n / probe.Rate;
               for (int c = 0; c < 2; c++) {
                  const double expected = centre + depth * std::sin(phi + (c ? offset : 0.0));
                  if ((n % period) < expected + 2) continue;
                  const double measured = double((n % period) - period / 2) - double(buffer[n * 2 + c]) / step;
                  matched &= std::abs(measured - expected) < 2e-3;
                  checked[k]++;
               }
            }
         }
      }
      AUDIO_CHECK(matched and checked[1] > 0 and checked[2] > 0 and checked[3] > 0);
   }

   // Mono ignores spread but keeps the stored value.
   const auto source = noise_buffer(9600, 1, 31);
   auto narrow = source, wide = source;
   ChorusFixture mono_a(48000, false, wet_settings(5, 12, 6, 0)), mono_b(48000, false, wet_settings(5, 12, 6, 100));
   mono_a.run(narrow, 64);
   mono_b.run(wide, 64);
   bool same = true;
   for (size_t i = 0; i < narrow.size(); i++) same &= narrow[i] IS wide[i];
   AUDIO_CHECK(same and mono_b.Processor.Settings.Spread IS 100);

   // Left-only input never reaches the right channel, at any spread.
   for (double spread : { 0.0, 50.0, 100.0 }) {
      ChorusFixture stereo(48000, true, wet_settings(8, 25, 9, spread));
      auto buffer = noise_buffer(9600, 2, 41);
      for (int i = 0; i < 9600; i++) buffer[i * 2 + 1] = 0;
      stereo.run(buffer, 128);
      bool isolated = true;
      for (int i = 0; i < 9600; i++) isolated &= buffer[i * 2 + 1] IS 0;
      AUDIO_CHECK(isolated);
   }
}

//********************************************************************************************************************
// The output never exceeds the largest input, the interpolation loss matches the analytic linear-interpolation
// response, and non-finite input is silence.

static void test_quality(AudioTestContext &Test)
{
   for (double mix : { 50.0, 100.0 }) {
      auto settings = wet_settings(10, 30, 9, 100);
      settings.Mix = mix;
      ChorusFixture fixture(192000, true, settings);
      auto buffer = noise_buffer(96000, 2, 3, 4.0);
      const double input_peak = peak(buffer);
      fixture.run(buffer, 256);
      bool finite = true;
      for (auto sample : buffer) finite &= std::isfinite(sample);
      AUDIO_CHECK(finite and peak(buffer) <= input_peak and peak(buffer) > 1.0);
   }

   // A half-frame static delay has the linear-interpolation response |cos(pi f / fs)|; whole frames are lossless.
   auto gain = [](double Frequency, double Delay) {
      ChorusFixture fixture(48000, false, wet_settings(1, Delay, 0));
      std::vector<float> buffer(9600);
      for (int i = 0; i < 9600; i++) {
         buffer[i] = float(0.5 * std::sin(2.0 * std::numbers::pi * Frequency * i / 48000.0));
      }
      double in = 0, out = 0;
      for (int i = 4800; i < 9600; i++) in += double(buffer[i]) * buffer[i];
      fixture.run(buffer);
      for (int i = 4800; i < 9600; i++) out += double(buffer[i]) * buffer[i];
      return std::sqrt(out / in);
   };
   kt::Log log("AudioTests");
   for (double frequency : { 1000.0, 5000.0, 10000.0, 16000.0 }) {
      const double half = gain(frequency, 10.0 + 0.5 / 48.0);
      const double analytic = std::cos(std::numbers::pi * frequency / 48000.0);
      AUDIO_CHECK(std::abs(half - analytic) < 2e-3 and half <= 1.0);
      AUDIO_CHECK(std::abs(gain(frequency, 10.0) - 1.0) < 1e-3);
      log.msg("Chorus interpolation loss at %.0f Hz, half-frame delay at 48 kHz: %.2f dB", frequency,
         20.0 * std::log10(half));
   }

   // Modulation sidebands: a 1 kHz tone through a 10 Hz sweep of 9 ms deviates in frequency by up to
   // 1000 * 2 pi * 10 * 0.009 = 565 Hz, so the wet output keeps its power but spreads it over neighbouring bins.
   {
      ChorusFixture fixture(48000, false, wet_settings(10, 20, 9));
      std::vector<float> buffer(48000);
      for (int i = 0; i < 48000; i++) buffer[i] = float(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * i / 48000.0));
      fixture.run(buffer, 256);
      double power = 0, carrier_re = 0, carrier_im = 0;
      for (int i = 4800; i < 48000; i++) {
         power += double(buffer[i]) * buffer[i];
         carrier_re += buffer[i] * std::cos(2.0 * std::numbers::pi * 1000.0 * i / 48000.0);
         carrier_im += buffer[i] * std::sin(2.0 * std::numbers::pi * 1000.0 * i / 48000.0);
      }
      const double frames = 43200.0;
      const double rms = std::sqrt(power / frames);
      const double carrier = 2.0 * std::hypot(carrier_re, carrier_im) / frames / 0.5;
      AUDIO_CHECK(std::abs(rms - 0.5 / std::sqrt(2.0)) < 0.01 and carrier < 0.5);
      log.msg("Chorus 1 kHz at 10 Hz rate, 9 ms depth: wet RMS %.4f, carrier %.1f dB", rms,
         20.0 * std::log10(carrier));
   }

   // Non-finite samples are silence on entry and do not hold the processor pending.
   ChorusFixture hostile(48000, true, wet_settings(10, 10, 9));
   std::vector<float> buffer(size_t(4800) * 2, 0.0f);
   buffer[0] = std::numeric_limits<float>::quiet_NaN();
   buffer[1] = std::numeric_limits<float>::infinity();
   hostile.run(buffer);
   bool silent = true;
   for (auto sample : buffer) silent &= sample IS 0;
   AUDIO_CHECK(silent and (not hostile.Processor.pending()));

   // The largest finite magnitudes pass through without overflow.
   std::fill(buffer.begin(), buffer.end(), std::numeric_limits<float>::max());
   for (int i = 0; i < 4800; i += 2) buffer[i * 2] = -std::numeric_limits<float>::max();
   hostile.run(buffer);
   bool finite = true;
   for (auto sample : buffer) finite &= std::isfinite(sample);
   AUDIO_CHECK(finite);
}

//********************************************************************************************************************
// Pending output covers the farthest possible read of the last input and ends exactly when that history expires.

static void test_tail(AudioTestContext &Test)
{
   // No input: immediately idle, although the oscillator runs.
   ChorusFixture empty(48000, true, ChorusSettings());
   std::vector<float> silence(size_t(4800) * 2, 0.0f);
   empty.run(silence);
   AUDIO_CHECK((not empty.Processor.pending()) and (empty.Processor.lfo_phase() > 0));

   // The sweep peaks at 39 ms exactly when the impulse reaches that distance: rate = 1 / (4 * 0.039 s).
   for (int rate : { 8000, 48000, 192000 }) {
      ChorusFixture fixture(rate, false, wet_settings(1.0 / 0.156, 30, 9));
      const int reach = rate * 39 / 1000, capacity = fixture.Processor.ring_capacity();
      AUDIO_CHECK(fixture.Processor.tail_frames() IS uint64_t(capacity));
      float sample = 1.0f;
      fixture.Processor.process(&sample, 1);
      bool held = true, after = true;
      int latest = 0;
      for (int i = 1; i <= capacity + 10; i++) {
         sample = 0;
         fixture.Processor.process(&sample, 1);
         if (i < capacity) held &= fixture.Processor.pending();
         else after &= not fixture.Processor.pending();
         if (i > capacity - 2) after &= sample IS 0;
         if (std::abs(sample) > 1e-3) latest = i;
         if (i IS reach) AUDIO_CHECK(sample > 0.99f);
      }
      AUDIO_CHECK(held and after and latest >= reach);
   }

   // The countdown restarts at every non-zero frame, with very quiet input treated like loud input.
   ChorusFixture quiet(48000, true, ChorusSettings());
   std::vector<float> faint(size_t(1000) * 2, 0.0f);
   faint[999 * 2 + 1] = 0x1p-40f;
   quiet.run(faint);
   std::vector<float> rest(size_t(1873) * 2, 0.0f);
   quiet.run(rest);
   AUDIO_CHECK(quiet.Processor.pending());
   float frame[2] = { 0, 0 };
   quiet.Processor.process(frame, 1);
   AUDIO_CHECK(not quiet.Processor.pending());

   // Expired history cannot be revealed by later edits during the silent gap.
   ChorusFixture fixture(48000, false, wet_settings(1, 10, 0));
   fixture.impulse(2000);
   AUDIO_CHECK(not fixture.Processor.pending());
   fixture.change(wet_settings(10, 30, 9));
   std::vector<float> after(4800, 0.0f);
   fixture.run(after);
   bool silent = true;
   for (auto sample : after) silent &= sample IS 0;
   AUDIO_CHECK(silent and (not fixture.Processor.pending()));

   // History recorded at zero mix is revealed when the mix rises during its lifetime.
   auto hidden = wet_settings(1, 20, 0);
   hidden.Mix = 0;
   ChorusFixture gap(48000, false, hidden);
   auto output = gap.impulse(100);
   AUDIO_CHECK(output[0] IS 1.0f and gap.Processor.pending());
   gap.change(wet_settings(1, 20, 0));
   output.assign(1000, 0.0f);
   gap.run(output);
   AUDIO_CHECK(output[860] IS 1.0f); // Frame 960 since the impulse, after the mix ramp has finished
}

//********************************************************************************************************************
// Reset clears history, transitions and phase and applies the committed settings; a rate change re-prepares.

static void test_lifecycle(AudioTestContext &Test)
{
   ChorusFixture fixture(48000, true, wet_settings(4, 10, 3));
   fixture.impulse(100);
   fixture.change(wet_settings(2, 20, 5));
   AUDIO_CHECK(fixture.Processor.pending() and fixture.Processor.crossfading() and fixture.Processor.lfo_phase() > 0);

   fixture.Processor.reset();
   AUDIO_CHECK((not fixture.Processor.pending()) and (not fixture.Processor.crossfading()));
   AUDIO_CHECK(fixture.Processor.lfo_phase() IS 0 and fixture.Processor.centre_delay() IS 960.0);
   AUDIO_CHECK(fixture.Processor.depth_frames() IS 240.0);
   std::vector<float> after(size_t(4800) * 2, 0.0f);
   fixture.run(after);
   bool silent = true;
   for (auto sample : after) silent &= sample IS 0;
   AUDIO_CHECK(silent);

   // An unsupported target on update leaves the settings committed for the next reset.
   const float *storage = fixture.Processor.ring_storage();
   fixture.Processor.update(wet_settings(1, 30, 1), nullptr);
   AUDIO_CHECK(fixture.Processor.Settings.Delay IS 30 and fixture.Processor.centre_delay() IS 960.0);
   fixture.Processor.reset();
   AUDIO_CHECK(fixture.Processor.centre_delay() IS 1440.0 and fixture.Processor.ring_storage() IS storage);

   // Reconfiguring for another rate replaces the storage and clears the history.
   fixture.impulse(10);
   std::unique_ptr<AudioEffectConfiguration> config;
   AUDIO_CHECK(fixture.Processor.prepare(96000, true, config) IS ERR::Okay);
   config->publish();
   fixture.Effect.OutputRate = 96000;
   fixture.Processor.reset();
   AUDIO_CHECK(fixture.Processor.ring_capacity() IS chorus_capacity(96000) and (not fixture.Processor.pending()));
   AUDIO_CHECK(fixture.Processor.centre_delay() IS 2880.0 and fixture.Processor.lfo_phase() IS 0);

   // A reset for a rate that does not match the prepared storage leaves the processor inactive.
   fixture.Effect.OutputRate = 48000;
   fixture.Processor.reset();
   AUDIO_CHECK(fixture.Processor.tail_frames() IS 0);
}

//********************************************************************************************************************
// Rate edits integrate into a continuous phase; the other controls ramp from their current values.

static void test_ramps(AudioTestContext &Test)
{
   ChorusFixture fixture(48000, true, wet_settings(1, 15, 3, 0));
   std::vector<float> buffer(size_t(1000) * 2, 0.0f);
   fixture.run(buffer, 37);
   const double cycle = 2.0 * std::numbers::pi;
   const double inc0 = cycle * 1.0 / 48000.0, inc1 = cycle * 10.0 / 48000.0;
   const double start = fixture.Processor.lfo_phase();
   AUDIO_CHECK(std::abs(start - inc0 * 1000) < 1e-9);

   fixture.change(wet_settings(10, 15, 9, 100));
   fixture.run(buffer, 37);
   // 480 ramp frames advance by R * inc0 + step * R (R - 1) / 2, then 520 frames at the new rate.
   const double step = (inc1 - inc0) / 480.0;
   const double expected = std::fmod(start + 480.0 * inc0 + step * 480.0 * 479.0 / 2.0 + 520.0 * inc1, cycle);
   AUDIO_CHECK(std::abs(fixture.Processor.lfo_phase() - expected) < 1e-9);
   AUDIO_CHECK(fixture.Processor.lfo_increment() IS inc1 and fixture.Processor.depth_frames() IS 432.0);
   AUDIO_CHECK(std::abs(fixture.Processor.phase_offset() - std::numbers::pi) < 1e-15);
   AUDIO_CHECK(fixture.Processor.wet_mix() IS 1.0);

   // A retarget mid-ramp starts from the current value.
   ChorusFixture ramp(48000, true, wet_settings(1, 15, 0, 0));
   ramp.change(wet_settings(1, 15, 8, 100));
   std::vector<float> half(size_t(240) * 2, 0.0f);
   ramp.run(half);
   const double midway = ramp.Processor.depth_frames();
   AUDIO_CHECK(std::abs(midway - 192.0) < 1e-9 and std::abs(ramp.Processor.phase_offset() - 0.5 * std::numbers::pi) <
      1e-9);
   ramp.change(wet_settings(1, 15, 0, 0));
   ramp.run(half);
   AUDIO_CHECK(std::abs(ramp.Processor.depth_frames() - midway * 0.5) < 1e-9);
   ramp.run(half);
   AUDIO_CHECK(ramp.Processor.depth_frames() IS 0 and ramp.Processor.phase_offset() IS 0);
   AUDIO_CHECK(not ramp.Processor.crossfading());

   // Rate, depth, spread and mix edits under a tone introduce no discontinuity; a step would approach the tone's
   // full amplitude, more than 20 times its sample-to-sample movement.  Ramping the modulation moves the read heads
   // faster for 10 ms, which raises the instantaneous pitch: with every control changed together the right head
   // moves at up to 0.5 (sweep) + 0.7 (depth ramp) + 2.8 (spread ramp) frames per frame, and the largest step grows
   // by about 2.6 times in the reference model.  Each control changed alone grows it by less than 1.4 times.
   struct Edit { ChorusSettings Settings; double Limit; };
   const auto initial = ChorusSettings { .Rate = 0.5, .Delay = 20, .Depth = 2, .Spread = 0, .Mix = 30 };
   const Edit edits[] = {
      { { .Rate = 9, .Delay = 20, .Depth = 2, .Spread = 0, .Mix = 90 }, 1.4 },
      { { .Rate = 0.5, .Delay = 20, .Depth = 9, .Spread = 0, .Mix = 30 }, 1.4 },
      { { .Rate = 0.5, .Delay = 20, .Depth = 2, .Spread = 100, .Mix = 30 }, 1.4 },
      { { .Rate = 9, .Delay = 20, .Depth = 9, .Spread = 100, .Mix = 90 }, 3.0 }
   };
   const int frames = 48000, edit_at = 24000;
   for (const auto &edit : edits) {
      ChorusFixture tone(48000, true, initial);
      std::vector<float> signal(size_t(frames) * 2);
      for (int i = 0; i < frames; i++) {
         signal[i * 2] = float(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * i / 48000.0));
         signal[i * 2 + 1] = float(0.5 * std::sin(2.0 * std::numbers::pi * 330.0 * i / 48000.0));
      }
      tone.Processor.process(signal.data(), edit_at);
      tone.change(edit.Settings);
      tone.Processor.process(signal.data() + edit_at * 2, frames - edit_at);
      auto max_step = [&](int Start, int End) {
         double result = 0;
         for (int i = Start; i < End; i++) {
            for (int c = 0; c < 2; c++) {
               result = std::max(result, double(std::abs(signal[i * 2 + c] - signal[(i - 1) * 2 + c])));
            }
         }
         return result;
      };
      AUDIO_CHECK(max_step(edit_at, edit_at + 960) < max_step(12000, edit_at) * edit.Limit);
   }
}

//********************************************************************************************************************
// Centre-delay edits crossfade two read heads that share the LFO: during the crossfade the output is exactly the
// linear blend of fixed-centre renders, and a single latest target is queued.

static void test_crossfade(AudioTestContext &Test)
{
   const int rate = 48000, frames = 4800, fade = 480;
   const auto source = noise_buffer(frames, 2, 51);
   auto fixed = [&](double Delay) {
      ChorusFixture fixture(rate, true, wet_settings(6, Delay, 7, 70));
      auto buffer = source;
      fixture.run(buffer, 64);
      return buffer;
   };
   const auto a = fixed(12), b = fixed(25), c = fixed(18);

   // Edit to 25 ms at frame 1000, then 30 ms and 18 ms while the first crossfade runs; only 18 ms is applied.
   ChorusFixture fixture(rate, true, wet_settings(6, 12, 7, 70));
   auto buffer = source;
   fixture.Processor.process(buffer.data(), 1000);
   fixture.change(wet_settings(6, 25, 7, 70));
   AUDIO_CHECK(fixture.Processor.crossfading() and fixture.Processor.centre_delay() IS 1200.0);
   fixture.Processor.process(buffer.data() + 2000, 100);
   fixture.change(wet_settings(6, 30, 7, 70));
   fixture.change(wet_settings(6, 18, 7, 70));
   AUDIO_CHECK(fixture.Processor.centre_queued() and fixture.Processor.centre_delay() IS 1200.0);
   fixture.Processor.process(buffer.data() + 2200, frames - 1100);
   AUDIO_CHECK((not fixture.Processor.crossfading()) and (not fixture.Processor.centre_queued()));

   auto expect = [&](int Frame, int Channel) -> double {
      const size_t i = size_t(Frame) * 2 + Channel;
      if (Frame < 1000) return a[i];
      if (Frame < 1000 + fade) return a[i] + (double(b[i]) - a[i]) * double(Frame - 1000) / fade;
      if (Frame < 1000 + 2 * fade) return b[i] + (double(c[i]) - b[i]) * double(Frame - 1000 - fade) / fade;
      return c[i];
   };
   double error = 0;
   for (int n = 0; n < frames; n++) {
      for (int ch = 0; ch < 2; ch++) error = std::max(error, std::abs(double(buffer[n * 2 + ch]) - expect(n, ch)));
   }
   AUDIO_CHECK(error < 1e-6);

   // An edit back to the crossfade's destination clears the queued target.
   fixture.change(wet_settings(6, 10, 7, 70));
   fixture.change(wet_settings(6, 20, 7, 70));
   AUDIO_CHECK(fixture.Processor.centre_queued());
   fixture.change(wet_settings(6, 10, 7, 70));
   AUDIO_CHECK(fixture.Processor.crossfading() and (not fixture.Processor.centre_queued()));
}

//********************************************************************************************************************
// An edit between the final crossfade frame and the next block supersedes the queued centre, including when the
// latest edit keeps the current centre.

static void test_crossfade_boundary(AudioTestContext &Test)
{
   for (int rate : { 8000, 48000, 192000 }) {
      const int fade = rate / 100;
      for (int channels = 1; channels <= 2; channels++) {
         for (double latest : { 10.0, 20.0 }) {
            ChorusFixture fixture(rate, channels IS 2, wet_settings(1, 15, 0));
            ChorusFixture reference(rate, channels IS 2, wet_settings(1, 15, 0));
            fixture.change(wet_settings(1, 20, 0));
            reference.change(wet_settings(1, 20, 0));
            fixture.change(wet_settings(1, 30, 0));
            auto a = noise_buffer(fade * 4, channels, 29), b = a;
            fixture.Processor.process(a.data(), fade);
            reference.Processor.process(b.data(), fade);
            AUDIO_CHECK((not fixture.Processor.crossfading()) and fixture.Processor.centre_queued());

            fixture.change(wet_settings(1, latest, 0));
            reference.change(wet_settings(1, latest, 0));
            AUDIO_CHECK(not fixture.Processor.centre_queued());
            fixture.Processor.process(a.data() + fade * channels, fade * 3);
            reference.Processor.process(b.data() + fade * channels, fade * 3);
            AUDIO_CHECK(a IS b);
            AUDIO_CHECK(fixture.Processor.centre_delay() IS latest * double(rate) / 1000.0);
         }
      }
   }
}

//********************************************************************************************************************
// Block boundaries do not change the output, including simultaneous edits and a queued centre.

static void test_blocks(AudioTestContext &Test)
{
   const auto initial = ChorusSettings { .Rate = 0.8, .Delay = 15, .Depth = 3, .Spread = 50, .Mix = 35 };
   const ChorusSettings edits[3] = {
      { .Rate = 9.5, .Delay = 28, .Depth = 9, .Spread = 100, .Mix = 70 },
      { .Rate = 0.05, .Delay = 10, .Depth = 0, .Spread = 0, .Mix = 100 }, // Queued behind the first crossfade
      { .Rate = 4, .Delay = 21.3, .Depth = 6.6, .Spread = 33, .Mix = 50 }
   };

   for (int rate : { 44100, 48000, 96000 }) {
      const int frames = rate / 2;
      auto source = noise_buffer(frames, 2, 3);
      for (int i = frames / 2; i < frames * 3 / 4; i++) source[i * 2] = source[i * 2 + 1] = 0; // Silent gap
      const int stops[] = { frames / 4, frames / 4 + 100, frames / 2 + 50, frames };
      auto render = [&](const std::vector<int> &Blocks) {
         ChorusFixture fixture(rate, true, initial);
         auto buffer = source;
         int position = 0, next = 0;
         for (int stage = 0; stage < 4; stage++) {
            while (position < stops[stage]) {
               const int count = std::min(Blocks[next++ % Blocks.size()], stops[stage] - position);
               fixture.Processor.process(buffer.data() + size_t(position) * 2, count);
               position += count;
            }
            if (stage < 3) fixture.change(edits[stage]);
         }
         return buffer;
      };

      const auto whole = render({ frames });
      bool identical = true;
      for (const auto &blocks : { std::vector<int>{ 1 }, std::vector<int>{ 17 }, std::vector<int>{ 256 },
            std::vector<int>{ 3, 250, 1, 64, 999 } }) {
         const auto split = render(blocks);
         for (size_t i = 0; i < whole.size(); i++) identical &= whole[i] IS split[i];
      }
      AUDIO_CHECK(identical and peak(whole) <= peak(source));
   }
}

//********************************************************************************************************************
// Storage never changes after preparation, and the worst-case render and reset costs at 192 kHz stereo are logged.

static void test_resources(AudioTestContext &Test)
{
   ChorusFixture fixture(192000, true, wet_settings(10, 30, 9, 100));
   const float *storage = fixture.Processor.ring_storage();
   const size_t samples = fixture.Processor.ring_samples();

   uint32_t seed = 17;
   std::vector<float> buffer(size_t(1024) * 2);
   double worst_block = 0;
   for (int block = 0; block < 400; block++) {
      // Overlapping transitions: a centre edit every block keeps both read heads active.
      fixture.change(wet_settings(0.05 + (block % 5) * 2.0, 10.0 + (block % 7) * 3.0, 9, (block & 1) ? 100 : 0));
      for (auto &sample : buffer) sample = float(noise(seed));
      const auto start = std::chrono::steady_clock::now();
      fixture.Processor.process(buffer.data(), 1024);
      const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start);
      worst_block = std::max(worst_block, elapsed.count());
   }

   const auto start = std::chrono::steady_clock::now();
   fixture.Processor.reset();
   const auto reset = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start);

   AUDIO_CHECK(fixture.Processor.ring_storage() IS storage and fixture.Processor.ring_samples() IS samples);
   kt::Log("AudioTests").msg("Chorus at 192 kHz stereo: %.1f KB ring, worst 1024-frame block %.1f us, reset %.2f us",
      double(samples * sizeof(float)) / 1024.0, worst_block, reset.count());
}

//********************************************************************************************************************

static void run(AudioTestContext &Test)
{
   test_configuration(Test);
   test_static_delay(Test);
   test_modulation(Test);
   test_quality(Test);
   test_tail(Test);
   test_lifecycle(Test);
   test_ramps(Test);
   test_crossfade(Test);
   test_crossfade_boundary(Test);
   test_blocks(Test);
   test_resources(Test);
}

} // namespace audio_tests_audio_chorus_dsp
