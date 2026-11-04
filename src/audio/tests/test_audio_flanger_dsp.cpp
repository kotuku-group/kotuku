// Included by audio.cpp to exercise the module implementation.

#include <chrono>

namespace audio_tests_audio_flanger_dsp {

// Configure exactly as activation does: prepare outside the lock, publish, then reset on the render thread.

struct FlangerFixture {
   extAudioEffect Effect{nullptr, 1};
   FlangerProcessor Processor;
   ERR Prepared;
   int Channels;

   FlangerFixture(int Rate, bool Stereo, const FlangerSettings &Settings) : Processor(&Effect, Settings) {
      Effect.OutputRate = Rate;
      Effect.Stereo = Stereo;
      Effect.ResetPending = false;
      Channels = Stereo ? 2 : 1;
      std::unique_ptr<AudioEffectConfiguration> config;
      Prepared = Processor.prepare(Rate, Stereo, config);
      if (config) config->publish();
      Processor.reset();
   }

   // Parameter updates derive their target on the control thread, as FlangerUpdate does.
   void change(const FlangerSettings &Settings) {
      FlangerTarget target;
      const bool valid = flanger_target(Settings, Processor.SampleRate, target);
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

static FlangerSettings wet_settings(double Rate, double Delay, double Depth, double Feedback = 0)
{
   return FlangerSettings { .Rate = Rate, .Delay = Delay, .Depth = Depth, .Feedback = Feedback, .Mix = 100 };
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

//********************************************************************************************************************
// An independent model of the published equations with unbounded history.  The phase is computed from the frame
// number rather than accumulated, and the recirculating signal is stored with float precision, as the ring is.

static std::vector<double> reference(const std::vector<float> &Input, int Channels, const FlangerSettings &Settings,
   int Rate)
{
   const int frames = int(Input.size()) / Channels;
   const double minimum = Settings.Delay * double(Rate) / 1000.0;
   const double depth = Settings.Depth * double(Rate) / 1000.0;
   const double g = Settings.Feedback / 100.0;
   const double m = Settings.Mix / 100.0;
   const double cycle = 2.0 * std::numbers::pi;

   std::vector<float> stored(Input.size(), 0.0f);
   auto past = [&](int Frame, int Channel) -> double {
      if (Frame < 0) return 0.0;
      return stored[size_t(Frame) * Channels + Channel];
   };

   std::vector<double> output(Input.size(), 0.0);
   for (int n = 0; n < frames; n++) {
      const double phi = std::fmod(double(n) * cycle * Settings.Rate / double(Rate), cycle);
      const double d = std::max(minimum + depth * 0.5 * (1.0 - std::cos(phi)), 1.0);
      const int whole = int(std::floor(d));
      const double fraction = d - double(whole);
      for (int c = 0; c < Channels; c++) {
         const float raw = Input[size_t(n) * Channels + c];
         const double x = std::isfinite(raw) ? double(raw) : 0.0;
         const double tap = past(n - whole, c) * (1.0 - fraction) + past(n - whole - 1, c) * fraction;
         const double w = x + g * tap;
         stored[size_t(n) * Channels + c] = (std::abs(w) < 1e-30) ? 0.0f : float(w);
         output[size_t(n) * Channels + c] = (1.0 - m) * x + m * tap;
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

static double peak(const std::vector<float> &Buffer, size_t Start = 0)
{
   double result = 0;
   for (size_t i = Start; i < Buffer.size(); i++) result = std::max(result, double(std::abs(Buffer[i])));
   return result;
}

static double rms(const std::vector<float> &Buffer, size_t Start, size_t End)
{
   double power = 0;
   for (size_t i = Start; i < End; i++) power += double(Buffer[i]) * Buffer[i];
   return std::sqrt(power / double(End - Start));
}

//********************************************************************************************************************

static void test_configuration(AudioTestContext &Test)
{
   FlangerFixture low(7999, false, FlangerSettings());
   AUDIO_CHECK(low.Prepared IS ERR::NoSupport);
   FlangerFixture high(192001, true, FlangerSettings());
   AUDIO_CHECK(high.Prepared IS ERR::NoSupport);

   // Supported endpoints allocate a ring covering 15 ms plus interpolation support.
   FlangerFixture slow(8000, false, FlangerSettings()), fast(192000, true, FlangerSettings());
   AUDIO_CHECK(slow.Prepared IS ERR::Okay and fast.Prepared IS ERR::Okay);
   AUDIO_CHECK(slow.Processor.ring_capacity() IS 122 and slow.Processor.ring_samples() IS 122);
   AUDIO_CHECK(fast.Processor.ring_capacity() IS 2882 and fast.Processor.ring_samples() IS 5764);
   AUDIO_CHECK(flanger_capacity(44100) IS 664); // 661.5 frames rounds up

   // An unconfigured processor passes audio through and reports no tail.
   FlangerFixture idle(0, true, FlangerSettings());
   AUDIO_CHECK(idle.Prepared IS ERR::Okay);
   float samples[] = { 0.25f, -0.5f, 0.75f, 1.0f };
   idle.Processor.process(samples, 2);
   AUDIO_CHECK(samples[0] IS 0.25f and samples[1] IS -0.5f and samples[2] IS 0.75f and samples[3] IS 1.0f);
   AUDIO_CHECK((not idle.Processor.pending()) and (idle.Processor.tail_frames() IS 0));
   AUDIO_CHECK(idle.Processor.decay_estimate() IS 0);

   // Zero algorithmic latency and a finite tail.  The defaults reach 3 ms (144 frames, 145 with interpolation); 30%
   // feedback needs 14 windows of 145 frames to fall below the residual, then the ring must be overwritten.
   FlangerFixture typical(48000, true, FlangerSettings());
   AUDIO_CHECK(typical.Processor.latency() IS 0 and typical.Processor.tail() IS AudioTail::FINITE);
   AUDIO_CHECK(typical.Processor.min_delay() IS 48.0 and typical.Processor.depth_frames() IS 96.0);
   AUDIO_CHECK(typical.Processor.feedback_gain() IS 0.3 and typical.Processor.wet_mix() IS 0.5);
   AUDIO_CHECK(typical.Processor.lfo_phase() IS 0);
   AUDIO_CHECK(typical.Processor.tail_frames() IS 145 * 14 + 722 + 1);
   AUDIO_CHECK(typical.Processor.decay_estimate() IS 145 * 7); // 0.3^6 is below -60 dB

   // Without feedback the tail is the readable history alone.
   FlangerFixture dry(48000, false, wet_settings(1, 5, 10));
   AUDIO_CHECK(dry.Processor.tail_frames() IS 723 and dry.Processor.decay_estimate() IS 721);

   // The bound does not depend on the sign of the feedback.
   FlangerFixture positive(48000, false, wet_settings(1, 2, 3, 75)), negative(48000, false, wet_settings(1, 2, 3, -75));
   AUDIO_CHECK(positive.Processor.tail_frames() IS negative.Processor.tail_frames());
   AUDIO_CHECK(positive.Processor.decay_estimate() IS negative.Processor.decay_estimate());
}

//********************************************************************************************************************
// At zero depth the wet path is a static fractional delay: matches an independent reference, exact at integer
// delays, clamped to one frame, and independent of rate.

static void test_static_delay(AudioTestContext &Test)
{
   // An integer delay reproduces the impulse exactly.
   {
      FlangerFixture fixture(48000, false, wet_settings(3, 2, 0));
      const auto output = fixture.impulse(1000);
      bool single = true;
      for (int i = 0; i < 1000; i++) if (i != 96) single &= output[i] IS 0;
      AUDIO_CHECK(single and output[96] IS 1.0f);
   }

   // A fractional delay spreads the impulse across two frames with linear weights that sum to one.
   {
      FlangerFixture fixture(8000, false, wet_settings(1, 1.03125, 0)); // 8.25 frames
      AUDIO_CHECK(fixture.Processor.min_delay() IS 8.25);
      const auto output = fixture.impulse(100);
      AUDIO_CHECK(output[7] IS 0 and output[8] IS 0.75f and output[9] IS 0.25f and output[10] IS 0);
   }

   // Below 10 kHz the shortest delays are clamped to one frame, in both the wet and feedback paths.
   {
      FlangerFixture fixture(8000, false, wet_settings(1, 0.1, 0, 50)); // 0.8 frames
      const auto output = fixture.impulse(20);
      bool repeats = output[0] IS 0;
      for (int i = 1; i < 20; i++) repeats &= output[i] IS float(std::ldexp(1.0, 1 - i));
      AUDIO_CHECK(repeats);
   }

   // Zero mix is the input, bit for bit, including headroom above unity; history and phase continue.
   for (int channels = 1; channels <= 2; channels++) {
      auto settings = wet_settings(10, 0.1, 10, 90);
      settings.Mix = 0;
      FlangerFixture fixture(48000, channels IS 2, settings);
      const auto input = noise_buffer(4801, channels, 7, 4.0);
      auto output = input;
      fixture.run(output, 17);
      bool identical = true;
      for (size_t i = 0; i < input.size(); i++) identical &= output[i] IS input[i];
      AUDIO_CHECK(identical and fixture.Processor.pending() and (fixture.Processor.lfo_phase() > 0));
   }

   // All-wet output removes the dry signal; a partial mix is the linear blend of the two.
   {
      FlangerFixture wet(48000, true, wet_settings(1, 2, 0));
      float frame[2] = { 0.5f, -0.25f };
      wet.Processor.process(frame, 1);
      AUDIO_CHECK(frame[0] IS 0 and frame[1] IS 0);

      auto settings = wet_settings(1, 5, 0);
      settings.Mix = 30;
      FlangerFixture partial(48000, false, settings);
      const auto output = partial.impulse(1000, 0.8f);
      AUDIO_CHECK(std::abs(output[0] - 0.56f) < 1e-7 and std::abs(output[240] - 0.24f) < 1e-7);
   }

   // Rate has no effect at zero depth.
   const auto input = noise_buffer(9600, 2, 21);
   auto first = input, second = input;
   FlangerFixture a(48000, true, wet_settings(0.02, 1.337, 0, 60)), b(48000, true, wet_settings(10, 1.337, 0, 60));
   a.run(first, 256);
   b.run(second, 17);
   AUDIO_CHECK(first IS second);

   // Every supported rate, both layouts, several mixes and feedback settings against the independent reference.
   for (int rate : { 8000, 44100, 48000, 96000, 192000 }) {
      for (int channels = 1; channels <= 2; channels++) {
         for (double mix : { 0.0, 50.0, 100.0 }) {
            for (double feedback : { 0.0, 60.0, -90.0 }) {
               auto settings = wet_settings(2, 1.73, 0, feedback);
               settings.Mix = mix;
               FlangerFixture fixture(rate, channels IS 2, settings);
               auto buffer = noise_buffer(rate / 20, channels, 11);
               const auto expected = reference(buffer, channels, settings, rate);
               fixture.run(buffer, 256);
               AUDIO_CHECK(max_error(buffer, expected) < 1e-5);
            }
         }
      }
   }
}

//********************************************************************************************************************
// Feedback recirculates the delayed signal: exact repeats at an integer delay, the analytic comb response, and the
// broadband gain stated in the documentation.

static void test_feedback(AudioTestContext &Test)
{
   // Repeats every 48 frames at g^(k - 1), exactly representable for these gains.
   for (double feedback : { 75.0, -50.0 }) {
      FlangerFixture fixture(48000, false, wet_settings(1, 1, 0, feedback));
      const auto output = fixture.impulse(48 * 12);
      bool exact = true;
      for (int i = 0; i < 48 * 12; i++) {
         const int k = i / 48;
         const double expected = ((i % 48) IS 0 and k > 0) ? std::pow(feedback / 100.0, k - 1) : 0.0;
         exact &= output[i] IS float(expected);
      }
      AUDIO_CHECK(exact);
   }

   // Comb response at a static 1 ms delay and 50% mix: H = (1 + z^-D / (1 - g z^-D)) / 2.  Notches fall at odd
   // multiples of 500 Hz and peaks at multiples of 1 kHz; negative feedback swaps the resonances.
   struct Point { double Feedback, Frequency, Gain; };
   const Point points[] = {
      { 0, 500, 0 }, { 0, 1000, 1 }, { 90, 1000, 5.5 }, { 90, 500, 0.5 * (1.0 - 1.0 / 1.9) },
      { -90, 500, 4.5 }, { -90, 1000, 0.5 * (1.0 + 1.0 / 1.9) }
   };
   kt::Log log("AudioTests");
   for (const auto &point : points) {
      auto settings = wet_settings(1, 1, 0, point.Feedback);
      settings.Mix = 50;
      FlangerFixture fixture(48000, false, settings);
      auto buffer = tone(48000, point.Frequency, 48000);
      fixture.run(buffer, 256);
      const double gain = rms(buffer, 24000, 48000) / (0.5 / std::sqrt(2.0));
      AUDIO_CHECK(std::abs(gain - point.Gain) < 1e-3 + point.Gain * 1e-3);
   }

   // White noise through the all-wet loop gains 1 / (1 - g^2) in power, about 7.2 dB at 90%.
   {
      FlangerFixture fixture(48000, false, wet_settings(1, 1, 0, 90));
      auto buffer = noise_buffer(480000, 1, 77);
      const double in = rms(buffer, 48000, buffer.size());
      fixture.run(buffer, 512);
      const double gain = rms(buffer, 48000, buffer.size()) / in;
      AUDIO_CHECK(std::abs(gain - std::sqrt(1.0 / (1.0 - 0.81))) < 0.05);
      log.msg("Flanger broadband wet gain at 90%% feedback: %.2f dB", 20.0 * std::log10(gain));
   }

   // Bounded output and storage at every extreme, including the sweep and maximum resonance.
   for (int rate : { 8000, 192000 }) {
      for (double feedback : { 90.0, -90.0 }) {
         for (double delay : { 0.1, 5.0 }) {
            FlangerFixture fixture(rate, true, wet_settings(10, delay, 10, feedback));
            auto buffer = noise_buffer(rate, 2, 5, 1.0);
            const double input_peak = peak(buffer);
            fixture.run(buffer, 256);
            const auto stored = fixture.Processor.ring_storage();
            double stored_peak = 0;
            for (size_t i = 0; i < fixture.Processor.ring_samples(); i++) {
               stored_peak = std::max(stored_peak, double(std::abs(stored[i])));
            }
            bool finite = true;
            for (auto sample : buffer) finite &= std::isfinite(sample);
            AUDIO_CHECK(finite and peak(buffer) <= FLANGER_HEADROOM * input_peak);
            AUDIO_CHECK(stored_peak <= FLANGER_HEADROOM * input_peak);
         }
      }
   }

   // A tone on the resonance approaches, but never exceeds, the headroom.
   {
      FlangerFixture fixture(48000, false, wet_settings(1, 1, 0, 90));
      auto buffer = tone(48000, 1000, 48000, 1.0);
      fixture.run(buffer, 256);
      const double resonance = peak(buffer, 24000);
      AUDIO_CHECK(resonance > 9.9 and resonance <= FLANGER_HEADROOM * 1.0001);
   }
}

//********************************************************************************************************************
// The modulated read position matches the reference at the rate, depth and delay extremes, with and without
// feedback.  Both channels share the LFO and remain isolated.

static void test_modulation(AudioTestContext &Test)
{
   const FlangerSettings cases[] = {
      wet_settings(0.02, 1, 2, 30), wet_settings(10, 5, 10, 90), wet_settings(10, 0.1, 10, -90),
      wet_settings(7.5, 2.2, 4.4, 0), { .Rate = 3.3, .Delay = 0.37, .Depth = 8.5, .Feedback = -45, .Mix = 50 }
   };
   for (int rate : { 8000, 44100, 48000, 96000, 192000 }) {
      for (int channels = 1; channels <= 2; channels++) {
         for (const auto &settings : cases) {
            FlangerFixture fixture(rate, channels IS 2, settings);
            auto buffer = noise_buffer(rate / 5, channels, 13);
            const auto expected = reference(buffer, channels, settings, rate);
            fixture.run(buffer, 256);
            AUDIO_CHECK(max_error(buffer, expected) < 1e-4);
         }
      }
   }

   // Known LFO positions, recovered without feedback from a sawtooth input whose linear interpolation is exact
   // between wraps.  The read distance at frame n is the sawtooth position minus the output.  Quarter cycles reach
   // the minimum delay (including the one-frame clamp at 8 kHz), the midpoint and the 15 ms maximum.
   struct Probe { int Rate; double Lfo, Delay, Depth; };
   const Probe probes[] = {
      { 48000, 10, 5, 10 }, { 48000, 10, 0.1, 10 }, { 8000, 0.02, 5, 10 }, { 8000, 10, 0.1, 10 },
      { 192000, 10, 5, 10 }
   };
   const int period = 16384;
   const double step = 1.0 / double(period);
   for (const auto &probe : probes) {
      FlangerFixture fixture(probe.Rate, false, wet_settings(probe.Lfo, probe.Delay, probe.Depth));
      const int quarter = int(std::lround(double(probe.Rate) / (4.0 * probe.Lfo)));
      const int frames = quarter * 11 + 3;
      std::vector<float> buffer(frames);
      for (int n = 0; n < frames; n++) buffer[n] = float(double((n % period) - period / 2) * step);
      fixture.run(buffer, 1000);

      const double minimum = probe.Delay * probe.Rate / 1000.0, depth = probe.Depth * probe.Rate / 1000.0;
      bool matched = true;
      int checked[4] = { 0, 0, 0, 0 };
      for (int cycle = 0; cycle <= 2; cycle++) {
         for (int k = 0; k <= 3; k++) {
            // The frames around each quarter cycle whose interpolation does not straddle a sawtooth wrap.
            const int middle = quarter * (cycle * 4 + k);
            if (middle < 2) continue;
            for (int n = middle - 2; n <= middle + 2; n++) {
               const double phi = 2.0 * std::numbers::pi * probe.Lfo * n / probe.Rate;
               const double expected = std::max(minimum + depth * 0.5 * (1.0 - std::cos(phi)), 1.0);
               if ((n % period) < expected + 2) continue;
               const double measured = double((n % period) - period / 2) - double(buffer[n]) / step;
               matched &= std::abs(measured - expected) < 2e-3;
               checked[k]++;
            }
         }
      }
      AUDIO_CHECK(matched and checked[0] > 0 and checked[1] > 0 and checked[2] > 0 and checked[3] > 0);
   }

   // Identical channels follow the same sweep, and match a mono render of the same signal.
   {
      const auto source = noise_buffer(9600, 1, 31);
      std::vector<float> stereo(9600 * 2);
      for (int i = 0; i < 9600; i++) stereo[i * 2] = stereo[i * 2 + 1] = source[i];
      auto mono = source;
      FlangerFixture a(48000, false, wet_settings(5, 1.2, 6, 70)), b(48000, true, wet_settings(5, 1.2, 6, 70));
      a.run(mono, 64);
      b.run(stereo, 64);
      bool same = true;
      for (int i = 0; i < 9600; i++) same &= (stereo[i * 2] IS mono[i]) and (stereo[i * 2 + 1] IS mono[i]);
      AUDIO_CHECK(same);
   }

   // Left-only input never reaches the right channel, at either feedback sign.
   for (double feedback : { 90.0, -90.0 }) {
      FlangerFixture stereo(48000, true, wet_settings(8, 2.5, 9, feedback));
      auto buffer = noise_buffer(9600, 2, 41);
      for (int i = 0; i < 9600; i++) buffer[i * 2 + 1] = 0;
      stereo.run(buffer, 128);
      bool isolated = true;
      for (int i = 0; i < 9600; i++) isolated &= buffer[i * 2 + 1] IS 0;
      AUDIO_CHECK(isolated);
   }
}

//********************************************************************************************************************
// Non-finite input is silence and the largest finite magnitudes cannot overflow the feedback loop.

static void test_hostile(AudioTestContext &Test)
{
   FlangerFixture hostile(48000, true, wet_settings(10, 0.1, 10, 90));
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
// Pending output covers the farthest possible read of the last input, and with feedback ends once the recirculated
// audio has decayed below the residual and left the ring, within the published bound.

static void test_tail(AudioTestContext &Test)
{
   // No input: immediately idle, although the oscillator runs.
   FlangerFixture empty(48000, true, FlangerSettings());
   std::vector<float> silence(size_t(4800) * 2, 0.0f);
   empty.run(silence);
   AUDIO_CHECK((not empty.Processor.pending()) and (empty.Processor.lfo_phase() > 0));

   // Without feedback, the sweep peaks at 15 ms half a cycle after reset.  An impulse placed 15 ms before that frame
   // is heard at the farthest reach, and pending ends exactly when the ring has been overwritten.
   for (int rate : { 8000, 48000, 192000 }) {
      FlangerFixture fixture(rate, false, wet_settings(10, 5, 10));
      const int half = rate / 20, reach = rate * 15 / 1000, capacity = fixture.Processor.ring_capacity();
      AUDIO_CHECK(fixture.Processor.tail_frames() IS uint64_t(capacity) + 1);
      std::vector<float> lead(half - reach, 0.0f);
      fixture.run(lead);
      AUDIO_CHECK(not fixture.Processor.pending());
      float sample = 1.0f;
      fixture.Processor.process(&sample, 1);
      bool held = true, after = true;
      for (int i = 1; i <= capacity + 10; i++) {
         sample = 0;
         fixture.Processor.process(&sample, 1);
         if (i < capacity) held &= fixture.Processor.pending();
         else after &= not fixture.Processor.pending();
         if (i > capacity - 2) after &= sample IS 0;
         if (i IS reach) AUDIO_CHECK(sample > 0.99f);
      }
      AUDIO_CHECK(held and after);
   }

   // With feedback at either sign, pending ends within the published bound and the output is silent afterwards.
   // Every write in the final ring length is below the residual, so every read there is below residual / |g|.
   kt::Log log("AudioTests");
   for (double feedback : { 90.0, -90.0, 30.0 }) {
      FlangerFixture fixture(48000, false, wet_settings(3, 5, 10, feedback));
      const uint64_t bound = fixture.Processor.tail_frames();
      const int capacity = fixture.Processor.ring_capacity();
      std::vector<float> output;
      float sample = 1.0f;
      uint64_t frames = 0;
      while (true) {
         fixture.Processor.process(&sample, 1);
         output.push_back(sample);
         sample = 0;
         if (not fixture.Processor.pending()) break;
         if (++frames > bound + 1000) break;
      }
      AUDIO_CHECK(frames <= bound and frames > uint64_t(capacity));
      AUDIO_CHECK(peak(output, output.size() - capacity) <= FLANGER_RESIDUAL / (std::abs(feedback) / 100.0));
      AUDIO_CHECK(peak(output, 0) > 0.5);
      std::vector<float> after(4800, 0.0f);
      fixture.run(after);
      AUDIO_CHECK(peak(after) IS 0);
      log.msg("Flanger %.0f%% feedback at 15 ms reach: pending %d frames, bound %d frames", feedback, int(frames),
         int(bound));
   }

   // The countdown restarts at every non-zero frame, with very quiet input treated like loud input.
   FlangerFixture quiet(48000, true, wet_settings(1, 1, 2, 0));
   std::vector<float> faint(size_t(1000) * 2, 0.0f);
   faint[999 * 2 + 1] = 0x1p-40f;
   quiet.run(faint);
   std::vector<float> rest(size_t(721) * 2, 0.0f);
   quiet.run(rest);
   AUDIO_CHECK(quiet.Processor.pending());
   float frame[2] = { 0, 0 };
   quiet.Processor.process(frame, 1);
   AUDIO_CHECK(not quiet.Processor.pending());

   // Expired history cannot be revealed by later edits during the silent gap.
   FlangerFixture fixture(48000, false, wet_settings(1, 1, 0, 50));
   fixture.impulse(4000);
   AUDIO_CHECK(not fixture.Processor.pending());
   fixture.change(wet_settings(10, 5, 10, 90));
   std::vector<float> after(4800, 0.0f);
   fixture.run(after);
   AUDIO_CHECK(peak(after) IS 0 and (not fixture.Processor.pending()));

   // The residual left in the ring by retirement is never read: new input matches a processor that never had any.
   {
      FlangerFixture fresh(48000, false, wet_settings(1, 1, 0, 50));
      std::vector<float> lead(4000, 0.0f);
      fresh.run(lead);
      fresh.change(wet_settings(10, 5, 10, 90));
      lead.assign(4800, 0.0f);
      fresh.run(lead);
      auto x = noise_buffer(2000, 1, 62), y = x;
      fixture.run(x, 64);
      fresh.run(y, 64);
      AUDIO_CHECK(x IS y);
   }

   // History recorded at zero mix is revealed when the mix rises during its lifetime.  The third pass arrives 720
   // frames after the impulse, after the mix ramp has finished.
   auto hidden = wet_settings(1, 5, 0, 50);
   hidden.Mix = 0;
   FlangerFixture gap(48000, false, hidden);
   auto output = gap.impulse(100);
   AUDIO_CHECK(output[0] IS 1.0f and gap.Processor.pending());
   gap.change(wet_settings(1, 5, 0, 50));
   output.assign(1000, 0.0f);
   gap.run(output);
   AUDIO_CHECK(output[620] IS 0.25f);
}

//********************************************************************************************************************
// A feedback reduction must not report a 60 dB decay while the previous gain is still ramping down.  Cover both
// signs, mono and stereo, and another edit during the ramp.

static void test_decay_ramp(AudioTestContext &Test)
{
   for (int rate : { 8000, 48000, 192000 }) {
      for (int channels = 1; channels <= 2; channels++) {
         for (double feedback : { 90.0, -90.0 }) {
            for (double target : { 0.0, feedback / 3.0 }) {
               for (bool retarget : { false, true }) {
                  FlangerFixture fixture(rate, channels IS 2, wet_settings(1, 1, 0, feedback));
                  fixture.impulse(1, 1.0f, 1.0f);
                  fixture.change(wet_settings(1, 1, 0, target));
                  if (retarget) {
                     std::vector<float> lead(size_t(rate / 500) * channels, 0.0f);
                     fixture.run(lead);
                     fixture.change(wet_settings(1, 1, 0, 0));
                  }

                  const auto estimate = fixture.Processor.decay_estimate();
                  const int frames = int(estimate) + fixture.Processor.ring_capacity();
                  std::vector<float> output(size_t(frames) * channels, 0.0f);
                  fixture.run(output, 37);
                  AUDIO_CHECK(peak(output) > 0.001);
                  AUDIO_CHECK(peak(output, size_t(estimate) * channels) <= 0.001);
               }
            }
         }
      }
   }
}

//********************************************************************************************************************
// Reset clears history, transitions and phase and applies the committed settings; a rate change re-prepares.

static void test_lifecycle(AudioTestContext &Test)
{
   FlangerFixture fixture(48000, true, wet_settings(4, 1, 3, 50));
   fixture.impulse(100);
   fixture.change(wet_settings(2, 2, 5, -20));
   AUDIO_CHECK(fixture.Processor.pending() and fixture.Processor.crossfading() and fixture.Processor.lfo_phase() > 0);

   fixture.Processor.reset();
   AUDIO_CHECK((not fixture.Processor.pending()) and (not fixture.Processor.crossfading()));
   AUDIO_CHECK(fixture.Processor.lfo_phase() IS 0 and fixture.Processor.min_delay() IS 96.0);
   AUDIO_CHECK(fixture.Processor.depth_frames() IS 240.0 and fixture.Processor.feedback_gain() IS -0.2);
   std::vector<float> after(size_t(4800) * 2, 0.0f);
   fixture.run(after);
   AUDIO_CHECK(peak(after) IS 0);

   // The ring is not cleared on reset, but history from before the reset is never read: new input after a reset
   // produces the same output as a freshly prepared processor.
   {
      FlangerFixture used(48000, true, wet_settings(3, 4, 10, 80)), fresh(48000, true, wet_settings(3, 4, 10, 80));
      auto loud = noise_buffer(4000, 2, 63, 1.0);
      used.run(loud);
      used.Processor.reset();
      auto x = noise_buffer(4000, 2, 64), y = x;
      used.run(x, 64);
      fresh.run(y, 64);
      AUDIO_CHECK(x IS y);
   }

   // An unsupported target on update leaves the settings committed for the next reset.
   const float *storage = fixture.Processor.ring_storage();
   fixture.Processor.update(wet_settings(1, 3, 1), nullptr);
   AUDIO_CHECK(fixture.Processor.Settings.Delay IS 3 and fixture.Processor.min_delay() IS 96.0);
   fixture.Processor.reset();
   AUDIO_CHECK(fixture.Processor.min_delay() IS 144.0 and fixture.Processor.ring_storage() IS storage);

   // Reconfiguring for another rate replaces the storage and clears the history.
   fixture.impulse(10);
   std::unique_ptr<AudioEffectConfiguration> config;
   AUDIO_CHECK(fixture.Processor.prepare(96000, true, config) IS ERR::Okay);
   config->publish();
   fixture.Effect.OutputRate = 96000;
   fixture.Processor.reset();
   AUDIO_CHECK(fixture.Processor.ring_capacity() IS flanger_capacity(96000) and (not fixture.Processor.pending()));
   AUDIO_CHECK(fixture.Processor.min_delay() IS 288.0 and fixture.Processor.lfo_phase() IS 0);

   // A reset for a rate that does not match the prepared storage leaves the processor inactive.
   fixture.Effect.OutputRate = 48000;
   fixture.Processor.reset();
   AUDIO_CHECK(fixture.Processor.tail_frames() IS 0);
}

//********************************************************************************************************************
// Rate edits integrate into a continuous phase; the other controls ramp from their current values.

static void test_ramps(AudioTestContext &Test)
{
   FlangerFixture fixture(48000, true, wet_settings(1, 1, 3, 0));
   std::vector<float> buffer(size_t(1000) * 2, 0.0f);
   fixture.run(buffer, 37);
   const double cycle = 2.0 * std::numbers::pi;
   const double inc0 = cycle * 1.0 / 48000.0, inc1 = cycle * 10.0 / 48000.0;
   const double start = fixture.Processor.lfo_phase();
   AUDIO_CHECK(std::abs(start - inc0 * 1000) < 1e-9);

   fixture.change(wet_settings(10, 1, 9, -90));
   fixture.run(buffer, 37);
   // 480 ramp frames advance by R * inc0 + step * R (R - 1) / 2, then 520 frames at the new rate.
   const double step = (inc1 - inc0) / 480.0;
   const double expected = std::fmod(start + 480.0 * inc0 + step * 480.0 * 479.0 / 2.0 + 520.0 * inc1, cycle);
   AUDIO_CHECK(std::abs(fixture.Processor.lfo_phase() - expected) < 1e-9);
   AUDIO_CHECK(fixture.Processor.lfo_increment() IS inc1 and fixture.Processor.depth_frames() IS 432.0);
   AUDIO_CHECK(fixture.Processor.feedback_gain() IS -0.9 and fixture.Processor.wet_mix() IS 1.0);

   // A retarget mid-ramp starts from the current value.
   FlangerFixture ramp(48000, true, wet_settings(1, 1, 0, -80));
   ramp.change(wet_settings(1, 1, 8, 80));
   std::vector<float> half(size_t(240) * 2, 0.0f);
   ramp.run(half);
   const double midway = ramp.Processor.depth_frames();
   AUDIO_CHECK(std::abs(midway - 192.0) < 1e-9 and std::abs(ramp.Processor.feedback_gain()) < 1e-9);
   ramp.change(wet_settings(1, 1, 0, 0));
   ramp.run(half);
   AUDIO_CHECK(std::abs(ramp.Processor.depth_frames() - midway * 0.5) < 1e-9);
   ramp.run(half);
   AUDIO_CHECK(ramp.Processor.depth_frames() IS 0 and ramp.Processor.feedback_gain() IS 0);
   AUDIO_CHECK(not ramp.Processor.crossfading());

   // Rate, depth, feedback and mix edits under a tone introduce no discontinuity; a step would approach the tone's
   // full amplitude, more than 20 times its sample-to-sample movement.  A depth ramp moves the read head faster for
   // 10 ms, raising the instantaneous pitch, and a feedback edit changes the resonance and so the level; the limits
   // cover both.
   struct Edit { FlangerSettings Settings; double Limit; };
   const auto initial = FlangerSettings { .Rate = 0.5, .Delay = 2, .Depth = 2, .Feedback = 30, .Mix = 30 };
   const Edit edits[] = {
      { { .Rate = 9, .Delay = 2, .Depth = 2, .Feedback = 30, .Mix = 90 }, 1.5 },
      { { .Rate = 0.5, .Delay = 2, .Depth = 10, .Feedback = 30, .Mix = 30 }, 1.5 },
      { { .Rate = 0.5, .Delay = 2, .Depth = 2, .Feedback = -90, .Mix = 30 }, 1.5 },
      { { .Rate = 9, .Delay = 2, .Depth = 10, .Feedback = -90, .Mix = 90 }, 2.0 }
   };
   const int frames = 48000, edit_at = 24000;
   kt::Log log("AudioTests");
   for (const auto &edit : edits) {
      FlangerFixture fixture_tone(48000, true, initial);
      std::vector<float> signal(size_t(frames) * 2);
      for (int i = 0; i < frames; i++) {
         signal[i * 2] = float(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * i / 48000.0));
         signal[i * 2 + 1] = float(0.5 * std::sin(2.0 * std::numbers::pi * 330.0 * i / 48000.0));
      }
      fixture_tone.Processor.process(signal.data(), edit_at);
      fixture_tone.change(edit.Settings);
      fixture_tone.Processor.process(signal.data() + edit_at * 2, frames - edit_at);
      auto max_step = [&](int Start, int End) {
         double result = 0;
         for (int i = Start; i < End; i++) {
            for (int c = 0; c < 2; c++) {
               result = std::max(result, double(std::abs(signal[i * 2 + c] - signal[(i - 1) * 2 + c])));
            }
         }
         return result;
      };
      const double ratio = max_step(edit_at, edit_at + 960) / max_step(12000, edit_at);
      AUDIO_CHECK(ratio < edit.Limit);
      log.msg("Flanger edit step ratio: %.2f", ratio);
   }
}

//********************************************************************************************************************
// Minimum delay edits crossfade two read heads that share the LFO: without feedback the output during the crossfade
// is exactly the linear blend of fixed-delay renders, and a single latest target is queued.

static void test_crossfade(AudioTestContext &Test)
{
   const int rate = 48000, frames = 4800, fade = 480;
   const auto source = noise_buffer(frames, 2, 51);
   auto fixed = [&](double Delay) {
      FlangerFixture fixture(rate, true, wet_settings(6, Delay, 7));
      auto buffer = source;
      fixture.run(buffer, 64);
      return buffer;
   };
   const auto a = fixed(0.5), b = fixed(4), c = fixed(2);

   // Edit to 4 ms at frame 1000, then 5 ms and 2 ms while the first crossfade runs; only 2 ms is applied.
   FlangerFixture fixture(rate, true, wet_settings(6, 0.5, 7));
   auto buffer = source;
   fixture.Processor.process(buffer.data(), 1000);
   fixture.change(wet_settings(6, 4, 7));
   AUDIO_CHECK(fixture.Processor.crossfading() and fixture.Processor.min_delay() IS 192.0);
   fixture.Processor.process(buffer.data() + 2000, 100);
   fixture.change(wet_settings(6, 5, 7));
   fixture.change(wet_settings(6, 2, 7));
   AUDIO_CHECK(fixture.Processor.delay_queued() and fixture.Processor.min_delay() IS 192.0);
   fixture.Processor.process(buffer.data() + 2200, frames - 1100);
   AUDIO_CHECK((not fixture.Processor.crossfading()) and (not fixture.Processor.delay_queued()));

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
   fixture.change(wet_settings(6, 1, 7));
   fixture.change(wet_settings(6, 3, 7));
   AUDIO_CHECK(fixture.Processor.delay_queued());
   fixture.change(wet_settings(6, 1, 7));
   AUDIO_CHECK(fixture.Processor.crossfading() and (not fixture.Processor.delay_queued()));

   // With feedback the crossfaded tap recirculates; the output stays within the headroom.
   FlangerFixture resonant(rate, true, wet_settings(6, 0.5, 7, 90));
   auto loud = noise_buffer(frames * 4, 2, 52, 1.0);
   for (int block = 0; block < 40; block++) {
      resonant.change(wet_settings(6, (block & 1) ? 5.0 : 0.1, 7, (block & 2) ? 90.0 : -90.0));
      resonant.Processor.process(loud.data() + size_t(block) * 480 * 2, 480);
   }
   AUDIO_CHECK(peak(loud) <= FLANGER_HEADROOM);
}

//********************************************************************************************************************
// An edit between the final crossfade frame and the next block supersedes the queued delay, including when the
// latest edit keeps the current delay.

static void test_crossfade_boundary(AudioTestContext &Test)
{
   for (int rate : { 8000, 48000, 192000 }) {
      const int fade = rate / 100;
      for (int channels = 1; channels <= 2; channels++) {
         for (double latest : { 1.0, 2.0 }) {
            FlangerFixture fixture(rate, channels IS 2, wet_settings(1, 1.5, 0, 40));
            FlangerFixture reference(rate, channels IS 2, wet_settings(1, 1.5, 0, 40));
            fixture.change(wet_settings(1, 2, 0, 40));
            reference.change(wet_settings(1, 2, 0, 40));
            fixture.change(wet_settings(1, 5, 0, 40));
            auto a = noise_buffer(fade * 4, channels, 29), b = a;
            fixture.Processor.process(a.data(), fade);
            reference.Processor.process(b.data(), fade);
            AUDIO_CHECK((not fixture.Processor.crossfading()) and fixture.Processor.delay_queued());

            fixture.change(wet_settings(1, latest, 0, 40));
            reference.change(wet_settings(1, latest, 0, 40));
            AUDIO_CHECK(not fixture.Processor.delay_queued());
            fixture.Processor.process(a.data() + fade * channels, fade * 3);
            reference.Processor.process(b.data() + fade * channels, fade * 3);
            AUDIO_CHECK(a IS b);
            AUDIO_CHECK(fixture.Processor.min_delay() IS latest * double(rate) / 1000.0);
         }
      }
   }
}

//********************************************************************************************************************
// Block boundaries do not change the output, including simultaneous edits, a queued delay and history retirement
// during a silent gap.

static void test_blocks(AudioTestContext &Test)
{
   const auto initial = FlangerSettings { .Rate = 0.25, .Delay = 1, .Depth = 2, .Feedback = 30, .Mix = 50 };
   const FlangerSettings edits[3] = {
      { .Rate = 9.5, .Delay = 4.5, .Depth = 10, .Feedback = -90, .Mix = 70 },
      { .Rate = 0.02, .Delay = 0.1, .Depth = 0, .Feedback = 0, .Mix = 100 }, // Queued behind the first crossfade
      { .Rate = 4, .Delay = 2.13, .Depth = 6.6, .Feedback = -40, .Mix = 50 }  // Retires during the silent gap
   };

   for (int rate : { 44100, 48000, 96000 }) {
      const int frames = rate / 2;
      auto source = noise_buffer(frames, 2, 3);
      for (int i = frames / 2; i < frames * 3 / 4; i++) source[i * 2] = source[i * 2 + 1] = 0; // Silent gap
      const int stops[] = { frames / 4, frames / 4 + 100, frames / 2 + 50, frames };
      bool retired = false;
      auto render = [&](const std::vector<int> &Blocks) {
         FlangerFixture fixture(rate, true, initial);
         auto buffer = source;
         int position = 0, next = 0;
         for (int stage = 0; stage < 4; stage++) {
            while (position < stops[stage]) {
               const int count = std::min(Blocks[next++ % Blocks.size()], stops[stage] - position);
               fixture.Processor.process(buffer.data() + size_t(position) * 2, count);
               position += count;
               if ((position < frames * 3 / 4) and (position > frames / 2)) retired |= not fixture.Processor.pending();
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
      AUDIO_CHECK(identical and retired and peak(whole) <= FLANGER_HEADROOM * peak(source));
   }
}

//********************************************************************************************************************
// Storage never changes after preparation, and the worst-case render and reset costs at 192 kHz stereo are logged.

static void test_resources(AudioTestContext &Test)
{
   FlangerFixture fixture(192000, true, wet_settings(10, 5, 10, 90));
   const float *storage = fixture.Processor.ring_storage();
   const size_t samples = fixture.Processor.ring_samples();

   uint32_t seed = 17;
   std::vector<float> buffer(size_t(1024) * 2);
   double worst_block = 0;
   for (int block = 0; block < 400; block++) {
      // Overlapping transitions: a delay edit every block keeps both read heads active.
      fixture.change(wet_settings(0.02 + (block % 5) * 2.0, 0.1 + (block % 7) * 0.8, 10, (block & 1) ? 90 : -90));
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
   kt::Log("AudioTests").msg("Flanger at 192 kHz stereo: %.1f KB ring, worst 1024-frame block %.1f us, reset %.2f us",
      double(samples * sizeof(float)) / 1024.0, worst_block, reset.count());
}

//********************************************************************************************************************

static void run(AudioTestContext &Test)
{
   test_configuration(Test);
   test_static_delay(Test);
   test_feedback(Test);
   test_modulation(Test);
   test_hostile(Test);
   test_tail(Test);
   test_decay_ramp(Test);
   test_lifecycle(Test);
   test_ramps(Test);
   test_crossfade(Test);
   test_crossfade_boundary(Test);
   test_blocks(Test);
   test_resources(Test);
}

} // namespace audio_tests_audio_flanger_dsp
