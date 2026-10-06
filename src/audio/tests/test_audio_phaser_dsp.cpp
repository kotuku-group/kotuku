// Included by audio.cpp to exercise the module implementation.

#include <chrono>

namespace audio_tests_audio_phaser_dsp {

// Configure exactly as activation does: prepare outside the lock, publish, then reset on the render thread.

struct PhaserFixture {
   extAudioEffect Effect{nullptr, 1};
   PhaserProcessor Processor;
   ERR Prepared;
   int Channels;

   PhaserFixture(int Rate, bool Stereo, const PhaserSettings &Settings) : Processor(&Effect, Settings) {
      Effect.OutputRate = Rate;
      Effect.Stereo = Stereo;
      Effect.ResetPending = false;
      Channels = Stereo ? 2 : 1;
      std::unique_ptr<AudioEffectConfiguration> config;
      Prepared = Processor.prepare(Rate, Stereo, config);
      if (config) config->publish();
      Processor.reset();
   }

   // Parameter updates derive their target on the control thread, as PhaserUpdate does.
   void change(const PhaserSettings &Settings) {
      PhaserTarget target;
      const bool valid = phaser_target(Settings, Processor.SampleRate, target);
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

static PhaserSettings settings(double Rate, double Centre, double Depth, double Feedback = 0, double Mix = 100)
{
   return PhaserSettings { .Rate = Rate, .Centre = Centre, .Depth = Depth, .Feedback = Feedback, .Mix = Mix };
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

static std::vector<float> tone(int Frames, double Frequency, int Rate, double Level = 0.5)
{
   std::vector<float> buffer(Frames);
   for (int i = 0; i < Frames; i++) buffer[i] = float(Level * std::sin(2.0 * std::numbers::pi * Frequency * i / Rate));
   return buffer;
}

static double peak(const std::vector<float> &Buffer, size_t Start = 0)
{
   double result = 0;
   for (size_t i = Start; i < Buffer.size(); i++) result = std::max(result, std::abs(double(Buffer[i])));
   return result;
}

static double rms(const std::vector<float> &Buffer, size_t Start, size_t End)
{
   double sum = 0;
   for (size_t i = Start; i < End; i++) sum += double(Buffer[i]) * double(Buffer[i]);
   return std::sqrt(sum / double(End - Start));
}

//********************************************************************************************************************
// An independent model of the published equations for fixed settings.  The phase is computed from the frame number
// rather than accumulated, and the coefficients are derived directly from tan() rather than the shared helpers.

static std::vector<double> reference(const std::vector<float> &Input, int Channels, const PhaserSettings &Settings,
   int Rate)
{
   const int frames = int(Input.size()) / Channels;
   const double g = Settings.Feedback / 100.0, mix = Settings.Mix / 100.0;
   const double octaves = 2.0 * Settings.Depth / 100.0;
   std::vector<double> output(Input.size());
   double s[2][6] = {}, w[2] = {};
   for (int i = 0; i < frames; i++) {
      const double phase = std::fmod(2.0 * std::numbers::pi * Settings.Rate / Rate * double(i), 2.0 * std::numbers::pi);
      double corner = Settings.Centre * std::pow(2.0, octaves * std::sin(phase));
      corner = std::min(std::max(corner, 20.0), 0.45 * Rate);
      const double t = std::tan(std::numbers::pi * corner / Rate);
      const double k = (1 - t) / (1 + t), q = std::sqrt(1 - k * k);
      for (int c = 0; c < Channels; c++) {
         const double x = Input[size_t(i) * Channels + c];
         double u = x + g * w[c];
         for (int j = 0; j < 6; j++) {
            const double y = q * s[c][j] - k * u;
            s[c][j] = k * s[c][j] + q * u;
            u = y;
         }
         w[c] = u;
         output[size_t(i) * Channels + c] = (1 - mix) * x + mix * u;
      }
   }
   return output;
}

static double max_error(const std::vector<float> &Actual, const std::vector<double> &Expected)
{
   double result = 0;
   for (size_t i = 0; i < Actual.size(); i++) result = std::max(result, std::abs(double(Actual[i]) - Expected[i]));
   return result;
}

//********************************************************************************************************************

static void test_configuration(AudioTestContext &Test)
{
   PhaserFixture low(7999, false, PhaserSettings());
   AUDIO_CHECK(low.Prepared IS ERR::NoSupport);
   PhaserFixture high(192001, true, PhaserSettings());
   AUDIO_CHECK(high.Prepared IS ERR::NoSupport);

   PhaserFixture slow(8000, false, PhaserSettings()), fast(192000, true, PhaserSettings());
   AUDIO_CHECK(slow.Prepared IS ERR::Okay and fast.Prepared IS ERR::Okay);
   AUDIO_CHECK(slow.Processor.pending() IS false and fast.Processor.tail_frames() > 0);

   // An unconfigured output leaves the processor inactive and the buffer unchanged.
   PhaserFixture idle(0, true, PhaserSettings());
   AUDIO_CHECK(idle.Prepared IS ERR::Okay);
   std::vector<float> samples = { 0.25f, -0.5f, 0.75f, 1.0f };
   idle.Processor.process(samples.data(), 2);
   AUDIO_CHECK(samples[0] IS 0.25f and samples[1] IS -0.5f and samples[2] IS 0.75f and samples[3] IS 1.0f);
   AUDIO_CHECK((not idle.Processor.pending()) and (idle.Processor.tail_frames() IS 0));
   AUDIO_CHECK(idle.Processor.decay_estimate() IS 0);

   // Defaults: an 800 Hz centre, 1.2 octaves either side, 20% feedback and an even mix.
   PhaserFixture typical(48000, true, PhaserSettings());
   AUDIO_CHECK(typical.Processor.latency() IS 0 and typical.Processor.tail() IS AudioTail::FINITE);
   AUDIO_CHECK(typical.Processor.centre_log2() IS std::log2(800.0));
   AUDIO_CHECK(std::abs(typical.Processor.sweep_octaves() - 1.2) < 1e-15);
   AUDIO_CHECK(typical.Processor.feedback_gain() IS 0.2 and typical.Processor.wet_mix() IS 0.5);
   AUDIO_CHECK(typical.Processor.lfo_phase() IS 0 and std::abs(typical.Processor.corner() - 800.0) < 1e-9);
   AUDIO_CHECK(typical.Processor.lfo_increment() IS 2.0 * std::numbers::pi * 0.4 / 48000.0);
   const double lowest = std::exp2(std::log2(800.0) - 1.2);
   AUDIO_CHECK(typical.Processor.decay_estimate() IS phaser_decay_estimate(lowest, 48000, 0.2));
   AUDIO_CHECK(typical.Processor.tail_frames() IS phaser_decay_bound(48000,
      std::min(phaser_q(lowest, 48000), phaser_q(std::exp2(std::log2(800.0) + 1.2), 48000)), 0.2));

   // The sign of the feedback does not affect the bounds.
   PhaserFixture positive(48000, false, settings(1, 300, 80, 70)), negative(48000, false, settings(1, 300, 80, -70));
   AUDIO_CHECK(positive.Processor.tail_frames() IS negative.Processor.tail_frames());
   AUDIO_CHECK(positive.Processor.decay_estimate() IS negative.Processor.decay_estimate());

   // The proven bound for the slowest settings stays inside the default Audio.MaxDrain of 30 seconds at every rate,
   // and stronger feedback or lower corners never shorten it.
   kt::Log log("AudioTests");
   for (int rate : { 8000, 44100, 48000, 96000, 192000 }) {
      PhaserFixture worst(rate, false, settings(0.02, 100, 100, 80));
      PhaserFixture milder(rate, false, settings(0.02, 100, 100, 40));
      PhaserFixture higher(rate, false, settings(0.02, 400, 50, 80));
      const auto bound = worst.Processor.tail_frames();
      AUDIO_CHECK(bound < uint64_t(rate) * 30);
      AUDIO_CHECK(milder.Processor.tail_frames() < bound and higher.Processor.tail_frames() < bound);
      AUDIO_CHECK(worst.Processor.decay_estimate() < bound);
      log.msg("Phaser at %d Hz: headroom bound %.0f, slowest tail bound %.2f s, decay estimate %.2f s", rate,
         phaser_headroom(rate), double(bound) / rate, double(worst.Processor.decay_estimate()) / rate);
   }
}

//********************************************************************************************************************
// At zero depth the cascade is a fixed all-pass: every frequency keeps its level, the impulse response keeps its
// energy, and an even mix cancels the corner frequency, where the cascade inverts the phase.

static void test_allpass(AudioTestContext &Test)
{
   for (double frequency : { 100.0, 800.0, 5000.0, 15000.0 }) {
      PhaserFixture fixture(48000, false, settings(1, 800, 0));
      auto buffer = tone(96000, frequency, 48000);
      const auto input = buffer;
      fixture.run(buffer);
      const double ratio = rms(buffer, 48000, 96000) / rms(input, 48000, 96000);
      AUDIO_CHECK(std::abs(ratio - 1.0) < 1e-3);
   }

   for (double centre : { 100.0, 800.0, 4000.0 }) {
      PhaserFixture fixture(48000, false, settings(1, centre, 0));
      auto output = fixture.impulse(48000);
      double energy = 0;
      for (auto sample : output) energy += double(sample) * double(sample);
      AUDIO_CHECK(std::abs(energy - 1.0) < 1e-5);
   }

   // The notch at the corner is complete; the response between notches is not.
   for (double centre : { 200.0, 800.0, 3000.0 }) {
      PhaserFixture notch(48000, false, settings(1, centre, 0, 0, 50));
      auto buffer = tone(48000, centre, 48000);
      notch.run(buffer);
      AUDIO_CHECK(rms(buffer, 24000, 48000) < 1e-4);

      PhaserFixture open(48000, false, settings(1, centre, 0, 0, 50));
      buffer = tone(48000, centre * 0.6, 48000);
      open.run(buffer);
      AUDIO_CHECK(rms(buffer, 24000, 48000) > 0.1);
   }

   // The lattice matches the direct-form all-pass (a + z^-1) / (1 + a z^-1), with a = -k, inside a feedback loop.
   for (double feedback : { 0.0, 60.0, -80.0 }) {
      const auto config = settings(1, 1200, 0, feedback, 50);
      PhaserFixture fixture(44100, false, config);
      auto buffer = noise_buffer(8000, 1, 7);
      const auto input = buffer;
      fixture.run(buffer, 37);

      const double t = std::tan(std::numbers::pi * 1200.0 / 44100.0), a = (t - 1) / (t + 1), g = feedback / 100.0;
      double xp[6] = {}, yp[6] = {}, w = 0, error = 0;
      for (size_t i = 0; i < input.size(); i++) {
         double u = input[i] + g * w;
         for (int j = 0; j < 6; j++) {
            const double y = a * u + xp[j] - a * yp[j];
            xp[j] = u;
            yp[j] = y;
            u = y;
         }
         w = u;
         error = std::max(error, std::abs(double(buffer[i]) - (0.5 * input[i] + 0.5 * u)));
      }
      AUDIO_CHECK(error < 1e-5);
   }
}

//********************************************************************************************************************
// The cascade passes DC unchanged and inverts nothing at Nyquist (six inversions), so a steady corner places the
// positive-feedback resonance at DC and the negative one at Nyquist, each with a gain of 1 / (1 - |g|).

static void test_feedback(AudioTestContext &Test)
{
   struct Point { double Feedback; bool Nyquist; double Gain; };
   for (auto point : { Point { 80, false, 5.0 }, Point { -80, false, 1.0 / 1.8 }, Point { -80, true, 5.0 },
         Point { 80, true, 1.0 / 1.8 }, Point { 50, false, 2.0 } }) {
      PhaserFixture fixture(48000, false, settings(1, 2000, 0, point.Feedback));
      std::vector<float> buffer(48000);
      for (size_t i = 0; i < buffer.size(); i++) buffer[i] = (point.Nyquist and (i & 1)) ? -0.1f : 0.1f;
      fixture.run(buffer);
      const double gain = std::abs(double(buffer.back())) / 0.1;
      AUDIO_CHECK(std::abs(gain - point.Gain) < 1e-4);
   }
}

//********************************************************************************************************************

static void test_modulation(AudioTestContext &Test)
{
   // The processor follows the published equations for mono and stereo, with and without feedback, at any block size.
   for (const auto &config : { settings(0.4, 800, 60, 20, 50), settings(10, 100, 100, 80, 100),
         settings(3, 4000, 100, -80, 70), settings(0.02, 1500, 25, 0, 30) }) {
      for (int rate : { 8000, 44100, 192000 }) {
         for (bool stereo : { false, true }) {
            PhaserFixture fixture(rate, stereo, config);
            auto buffer = noise_buffer(rate / 4, fixture.Channels, 21);
            const auto expected = reference(buffer, fixture.Channels, config, rate);
            fixture.run(buffer, 113);
            AUDIO_CHECK(max_error(buffer, expected) < 1e-4);
         }
      }
   }

   // The instantaneous corner follows the sine sweep from the centre and is clamped to 45% of the output rate.
   PhaserFixture sweep(48000, false, settings(1, 1000, 50));
   std::vector<float> silence(12000, 0.0f);
   sweep.run(silence);
   AUDIO_CHECK(std::abs(sweep.Processor.corner() - 2000.0) < 1e-6); // A quarter cycle: one octave up

   PhaserFixture clamped(8000, false, settings(1, 4000, 100));
   double highest = 0;
   for (int i = 0; i < 8000; i++) {
      float sample = 0;
      clamped.Processor.process(&sample, 1);
      highest = std::max(highest, clamped.Processor.corner());
   }
   AUDIO_CHECK(highest IS 3600.0);

   // Both channels share the oscillator, but no audio passes between them.
   PhaserFixture stereo(48000, true, settings(2, 500, 80, 80));
   auto output = stereo.impulse(4800, 1.0f, 0.0f);
   double right = 0;
   for (size_t i = 1; i < output.size(); i += 2) right = std::max(right, std::abs(double(output[i])));
   AUDIO_CHECK(right IS 0 and peak(output) > 0.1);
}

//********************************************************************************************************************
// Under the fastest, widest sweep and alternating maximum feedback the stored energy stays within the proven bound,
// and once the input ends it never increases, even while every parameter is ramping.

static void test_stability(AudioTestContext &Test)
{
   for (int rate : { 8000, 192000 }) {
      PhaserFixture fixture(rate, true, settings(10, 100, 100, 80));
      const double bound = phaser_headroom(rate);
      uint32_t seed = 5;
      double worst = 0;
      bool finite = true;
      std::vector<float> buffer(size_t(256) * 2);
      for (int block = 0; block < 400; block++) {
         fixture.change(settings(0.02 + (block % 5) * 2.4, (block % 3) ? 100 : 4000, 100, (block & 1) ? 80 : -80));
         for (auto &sample : buffer) sample = float(noise(seed));
         fixture.Processor.process(buffer.data(), 256);
         for (auto sample : buffer) finite &= std::isfinite(sample);
         worst = std::max(worst, std::sqrt(fixture.Processor.stored_energy()));
      }
      AUDIO_CHECK(finite and worst <= bound);

      bool decreasing = true;
      double last = fixture.Processor.stored_energy();
      for (int frame = 0; frame < rate; frame++) {
         if (frame % 97 IS 0) {
            fixture.change(settings(10 - (frame % 3) * 4.0, 100 + (frame % 7) * 500.0, (frame % 11) * 10.0,
               (frame & 1) ? 80 : -80, (frame % 5) * 25.0));
         }
         float frame_buffer[2] = { 0, 0 };
         fixture.Processor.process(frame_buffer, 1);
         const double energy = fixture.Processor.stored_energy();
         decreasing &= energy <= last * (1.0 + 1e-12);
         last = energy;
      }
      AUDIO_CHECK(decreasing);
      kt::Log("AudioTests").msg("Phaser at %d Hz under hostile modulation: state %.2f x input, bound %.0f", rate,
         worst, bound);
   }
}

//********************************************************************************************************************
// Non-finite input is silence and the largest finite magnitudes cannot overflow the output.

static void test_hostile(AudioTestContext &Test)
{
   PhaserFixture hostile(48000, true, settings(10, 100, 100, 80));
   std::vector<float> buffer(size_t(4800) * 2, 0.0f);
   buffer[0] = std::numeric_limits<float>::quiet_NaN();
   buffer[1] = std::numeric_limits<float>::infinity();
   hostile.run(buffer);
   bool silent = true;
   for (auto sample : buffer) silent &= sample IS 0;
   AUDIO_CHECK(silent and (not hostile.Processor.pending()));

   std::fill(buffer.begin(), buffer.end(), std::numeric_limits<float>::max());
   for (int i = 0; i < 4800; i += 2) buffer[i * 2] = -std::numeric_limits<float>::max();
   hostile.run(buffer);
   bool finite = true;
   for (auto sample : buffer) finite &= std::isfinite(sample);
   AUDIO_CHECK(finite);
}

//********************************************************************************************************************
// Pending output ends once the stored energy is below the residual, within the published bound, and the output is
// silent afterwards.

static void test_tail(AudioTestContext &Test)
{
   // No input: immediately idle, although the oscillator runs.
   PhaserFixture empty(48000, true, PhaserSettings());
   std::vector<float> silence(size_t(4800) * 2, 0.0f);
   empty.run(silence);
   AUDIO_CHECK((not empty.Processor.pending()) and (empty.Processor.lfo_phase() > 0));

   kt::Log log("AudioTests");
   for (const auto &config : { settings(0.4, 800, 60, 20), settings(0.02, 100, 0, 80), settings(0.02, 100, 0, -80),
         settings(10, 100, 100, 80), settings(1, 4000, 100, 0) }) {
      PhaserFixture fixture(48000, false, config);
      const uint64_t bound = fixture.Processor.tail_frames();
      float sample = 1.0f;
      uint64_t frames = 0;
      double first = 0;
      while (true) {
         fixture.Processor.process(&sample, 1);
         first = std::max(first, std::abs(double(sample)));
         sample = 0;
         if (not fixture.Processor.pending()) break;
         if (++frames > bound + 1000) break;
      }
      AUDIO_CHECK(frames <= bound and first > 0.1);
      AUDIO_CHECK(fixture.Processor.stored_energy() IS 0);
      std::vector<float> after(4800, 0.0f);
      fixture.run(after);
      AUDIO_CHECK(peak(after) IS 0);
      log.msg("Phaser centre %.0f Hz, depth %.0f%%, feedback %.0f%%: pending %.3f s, bound %.3f s, estimate %.3f s",
         config.Centre, config.Depth, config.Feedback, double(frames) / 48000, double(bound) / 48000,
         double(fixture.Processor.decay_estimate()) / 48000);
   }

   // Very quiet input is treated like loud input.
   PhaserFixture quiet(48000, true, settings(1, 800, 50, 50));
   std::vector<float> faint(size_t(100) * 2, 0.0f);
   faint[99 * 2 + 1] = 0x1p-40f;
   quiet.run(faint);
   AUDIO_CHECK(quiet.Processor.pending());

   // Expired history cannot be revealed by later edits during the silent gap.
   PhaserFixture fixture(48000, false, settings(1, 2000, 0, 0));
   fixture.impulse(48000);
   AUDIO_CHECK(not fixture.Processor.pending());
   fixture.change(settings(10, 100, 100, 80));
   std::vector<float> after(4800, 0.0f);
   fixture.run(after);
   AUDIO_CHECK(peak(after) IS 0 and (not fixture.Processor.pending()));

   // History recorded at zero mix is revealed when the mix rises during its lifetime.
   PhaserFixture gap(48000, false, settings(1, 100, 0, 80, 0));
   auto output = gap.impulse(100);
   AUDIO_CHECK(output[0] IS 1.0f and peak(output, 1) IS 0 and gap.Processor.pending());
   gap.change(settings(1, 100, 0, 80, 100));
   output.assign(4800, 0.0f);
   gap.run(output);
   AUDIO_CHECK(peak(output, 480) > 1e-3);
}

//********************************************************************************************************************
// Edits ramp over 10 ms from the current values, preserving the phase and the history.

static void test_ramps(AudioTestContext &Test)
{
   PhaserFixture fixture(48000, false, settings(1, 800, 50, 20, 50));
   fixture.impulse(200);
   const double phase = fixture.Processor.lfo_phase();
   fixture.change(settings(5, 3200, 100, -60, 100));
   AUDIO_CHECK(fixture.Processor.pending() and fixture.Processor.lfo_phase() IS phase);
   AUDIO_CHECK(fixture.Processor.centre_log2() IS std::log2(800.0) and fixture.Processor.wet_mix() IS 0.5);

   std::vector<float> half(240, 0.0f);
   fixture.run(half);
   AUDIO_CHECK(std::abs(fixture.Processor.centre_log2() - (std::log2(800.0) + 1.0)) < 1e-9); // Halfway in octaves
   AUDIO_CHECK(std::abs(fixture.Processor.feedback_gain() + 0.2) < 1e-9);
   AUDIO_CHECK(std::abs(fixture.Processor.wet_mix() - 0.75) < 1e-9);
   AUDIO_CHECK(std::abs(fixture.Processor.sweep_octaves() - 1.5) < 1e-9);

   fixture.run(half);
   AUDIO_CHECK(fixture.Processor.centre_log2() IS std::log2(3200.0) and fixture.Processor.feedback_gain() IS -0.6);
   AUDIO_CHECK(fixture.Processor.wet_mix() IS 1.0 and fixture.Processor.sweep_octaves() IS 2.0);
   AUDIO_CHECK(fixture.Processor.lfo_increment() IS 2.0 * std::numbers::pi * 5.0 / 48000.0);

   // A ramped edit produces no step: consecutive output samples of a steady tone stay close.
   PhaserFixture smooth(48000, false, settings(0.5, 500, 20, 0, 0));
   auto buffer = tone(4800, 200, 48000);
   std::vector<float> first(buffer.begin(), buffer.begin() + 2400), second(buffer.begin() + 2400, buffer.end());
   smooth.run(first);
   smooth.change(settings(0.5, 500, 20, 80, 100));
   smooth.run(second);
   double jump = 0;
   for (size_t i = 1; i < second.size(); i++) jump = std::max(jump, std::abs(double(second[i]) - second[i - 1]));
   AUDIO_CHECK(jump < 0.1);

   // Updates are ignored while a reset is pending or the rate does not match, but the settings are kept.
   fixture.Effect.ResetPending = true;
   fixture.change(settings(2, 1000, 10));
   AUDIO_CHECK(fixture.Processor.Settings.Centre IS 1000 and fixture.Processor.centre_log2() IS std::log2(3200.0));
   fixture.Effect.ResetPending = false;
   fixture.Processor.update(settings(2, 1500, 10), nullptr);
   AUDIO_CHECK(fixture.Processor.Settings.Centre IS 1500 and fixture.Processor.centre_log2() IS std::log2(3200.0));
}

//********************************************************************************************************************

static void test_lifecycle(AudioTestContext &Test)
{
   PhaserFixture fixture(48000, true, settings(4, 1000, 30, 50));
   fixture.impulse(100);
   fixture.change(settings(2, 2000, 50, -20));
   AUDIO_CHECK(fixture.Processor.pending() and fixture.Processor.lfo_phase() > 0);

   fixture.Processor.reset();
   AUDIO_CHECK((not fixture.Processor.pending()) and fixture.Processor.lfo_phase() IS 0);
   AUDIO_CHECK(fixture.Processor.centre_log2() IS std::log2(2000.0) and fixture.Processor.feedback_gain() IS -0.2);
   std::vector<float> after(size_t(4800) * 2, 0.0f);
   fixture.run(after);
   AUDIO_CHECK(peak(after) IS 0);

   // New input after a reset produces the same output as a freshly prepared processor.
   {
      PhaserFixture used(48000, true, settings(3, 400, 80, 80)), fresh(48000, true, settings(3, 400, 80, 80));
      auto loud = noise_buffer(4000, 2, 63, 1.0);
      used.run(loud);
      used.Processor.reset();
      auto x = noise_buffer(4000, 2, 64), y = x;
      used.run(x, 64);
      fresh.run(y, 64);
      AUDIO_CHECK(x IS y);
   }

   // Reconfiguring for another rate clears the history.
   fixture.impulse(10);
   std::unique_ptr<AudioEffectConfiguration> config;
   AUDIO_CHECK(fixture.Processor.prepare(96000, true, config) IS ERR::Okay);
   config->publish();
   AUDIO_CHECK(not fixture.Processor.pending());
   fixture.Effect.OutputRate = 96000;
   fixture.Processor.reset();
   AUDIO_CHECK(fixture.Processor.lfo_increment() IS 2.0 * std::numbers::pi * 2.0 / 96000.0);

   // A reset for a rate that does not match the prepared rate, or for a wider layout, leaves the processor inactive.
   fixture.Effect.OutputRate = 48000;
   fixture.Processor.reset();
   std::vector<float> unchanged = { 0.5f, -0.5f };
   fixture.Processor.process(unchanged.data(), 1);
   AUDIO_CHECK(unchanged[0] IS 0.5f and unchanged[1] IS -0.5f and fixture.Processor.tail_frames() IS 0);

   fixture.Effect.OutputRate = 96000;
   fixture.Effect.Layout = { 1, 2, 3 };
   fixture.Processor.reset();
   AUDIO_CHECK(fixture.Processor.tail_frames() IS 0 and fixture.Processor.decay_estimate() IS 0);
}

//********************************************************************************************************************
// Output is independent of how the frames are divided into blocks.

static void test_blocks(AudioTestContext &Test)
{
   const auto config = settings(7, 300, 90, -70, 60);
   const auto source = noise_buffer(9600, 2, 11);
   auto render = [&](const std::vector<int> &Blocks) {
      PhaserFixture fixture(44100, true, config);
      auto buffer = source;
      size_t frame = 0, index = 0;
      while (frame < 9600) {
         const size_t count = std::min(size_t(Blocks[index++ % Blocks.size()]), size_t(9600) - frame);
         fixture.Processor.process(buffer.data() + frame * 2, int(count));
         frame += count;
      }
      return buffer;
   };
   const auto whole = render({ 9600 });
   bool identical = true;
   for (const auto &blocks : { std::vector<int>{ 1 }, std::vector<int>{ 17 },
         std::vector<int>{ 3, 250, 1, 64, 999 } }) {
      const auto split = render(blocks);
      for (size_t i = 0; i < whole.size(); i++) identical &= whole[i] IS split[i];
   }
   AUDIO_CHECK(identical);
}

//********************************************************************************************************************
// The worst-case render cost at 192 kHz stereo is logged.

static void test_resources(AudioTestContext &Test)
{
   PhaserFixture fixture(192000, true, settings(10, 100, 100, 80));
   uint32_t seed = 17;
   std::vector<float> buffer(size_t(1024) * 2);
   double worst_block = 0;
   for (int block = 0; block < 400; block++) {
      fixture.change(settings(0.02 + (block % 5) * 2.0, 100 + (block % 7) * 500.0, 100, (block & 1) ? 80 : -80));
      for (auto &sample : buffer) sample = float(noise(seed));
      const auto start = std::chrono::steady_clock::now();
      fixture.Processor.process(buffer.data(), 1024);
      const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start);
      worst_block = std::max(worst_block, elapsed.count());
   }
   AUDIO_CHECK(sizeof(PhaserProcessor) < 512);
   kt::Log("AudioTests").msg("Phaser at 192 kHz stereo: worst 1024-frame block %.1f us, processor %d bytes",
      worst_block, int(sizeof(PhaserProcessor)));
}

//********************************************************************************************************************

static void run(AudioTestContext &Test)
{
   test_configuration(Test);
   test_allpass(Test);
   test_feedback(Test);
   test_modulation(Test);
   test_stability(Test);
   test_hostile(Test);
   test_tail(Test);
   test_ramps(Test);
   test_lifecycle(Test);
   test_blocks(Test);
   test_resources(Test);
}

} // namespace audio_tests_audio_phaser_dsp
