// Included by audio.cpp to exercise the module implementation.

#include <chrono>

namespace audio_tests_audio_stereo_dsp {

// Configure exactly as activation does: prepare outside the lock, publish, then reset on the render thread.

struct StereoFixture {
   extAudioEffect Effect{nullptr, 1};
   StereoProcessor Processor;
   ERR Prepared;
   int Channels;

   StereoFixture(int Rate, bool Stereo, const StereoSettings &Settings) : Processor(&Effect, Settings) {
      Effect.OutputRate = Rate;
      Effect.Stereo = Stereo;
      Effect.ResetPending = false;
      Channels = Stereo ? 2 : 1;
      std::unique_ptr<AudioEffectConfiguration> config;
      Prepared = Processor.prepare(Rate, Stereo, config);
      if (config) config->publish();
      Processor.reset();
   }

   // Parameter updates derive their target on the control thread, as StereoUpdate does.
   void change(const StereoSettings &Settings) {
      StereoTarget target;
      const bool valid = stereo_target(Settings, Processor.SampleRate, target);
      Processor.update(Settings, valid ? &target : nullptr);
   }

   void run(std::vector<float> &Buffer, int Block = 0) {
      const int frames = int(Buffer.size()) / Channels;
      if (Block <= 0) Block = frames;
      for (int i = 0; i < frames; i += Block) {
         Processor.process(Buffer.data() + size_t(i) * Channels, std::min(Block, frames - i));
      }
   }

   // Frames of silence processed until the processor stops reporting pending, or Limit.
   int drain(int Limit) {
      std::vector<float> silence(size_t(Channels), 0.0f);
      int frames = 0;
      while (Processor.pending() and (frames < Limit)) {
         std::fill(silence.begin(), silence.end(), 0.0f);
         Processor.process(silence.data(), 1);
         frames++;
      }
      return frames;
   }
};

static StereoSettings settings(double Width, double Balance = 0, bool Swap = false, bool Split = false,
   double Crossover = 150, double BassWidth = 0, double Gain = 0)
{
   return StereoSettings { .Width = Width, .Balance = Balance, .Swap = Swap, .Split = Split, .Crossover = Crossover,
      .BassWidth = BassWidth, .Gain = Gain };
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

// Interleaved stereo tones: the mid signal at MidFrequency and the side signal at SideFrequency.

static std::vector<float> mid_side_tones(int Frames, int Rate, double MidFrequency, double MidLevel,
   double SideFrequency, double SideLevel)
{
   std::vector<float> buffer(size_t(Frames) * 2);
   for (int i = 0; i < Frames; i++) {
      const double m = MidLevel * std::sin(2.0 * std::numbers::pi * MidFrequency * i / Rate);
      const double s = SideLevel * std::sin(2.0 * std::numbers::pi * SideFrequency * i / Rate);
      buffer[size_t(i) * 2] = float(m + s);
      buffer[size_t(i) * 2 + 1] = float(m - s);
   }
   return buffer;
}

static double peak(const std::vector<float> &Buffer, size_t Start = 0)
{
   double result = 0;
   for (size_t i = Start; i < Buffer.size(); i++) result = std::max(result, std::abs(double(Buffer[i])));
   return result;
}

// RMS of the side signal of an interleaved stereo buffer, from frame Start to End.

static double side_rms(const std::vector<float> &Buffer, size_t Start, size_t End)
{
   double sum = 0;
   for (size_t i = Start; i < End; i++) {
      const double s = (double(Buffer[i * 2]) - double(Buffer[i * 2 + 1])) * 0.5;
      sum += s * s;
   }
   return std::sqrt(sum / double(End - Start));
}

static double max_error(const std::vector<float> &Actual, const std::vector<double> &Expected)
{
   double result = 0;
   for (size_t i = 0; i < Actual.size(); i++) result = std::max(result, std::abs(double(Actual[i]) - Expected[i]));
   return result;
}

//********************************************************************************************************************
// An independent model of the published equations for fixed settings.  The Butterworth filter is derived from the
// analog prototype s^2 / (s^2 + sqrt(2) s + 1) with K = tan(pi fc / fs), rather than the processor's RBJ form, the
// split is computed from its two bands, and the swap exchanges the inputs literally.  The history is never discarded.

static std::vector<double> reference(const std::vector<float> &Input, const StereoSettings &Settings, int Rate)
{
   const int frames = int(Input.size()) / 2;
   const double w = Settings.Width / 100.0, b = Settings.BassWidth / 100.0;
   const double p = Settings.Balance / 100.0;
   const double gl = (p > 0) ? 1 - p : 1, gr = (p < 0) ? 1 + p : 1;
   const double t = std::pow(10.0, Settings.Gain / 20.0);
   const double k = std::tan(std::numbers::pi * Settings.Crossover / Rate);
   const double norm = 1.0 / (1.0 + std::sqrt(2.0) * k + k * k);
   const double b0 = norm, b1 = -2.0 * norm, b2 = norm;
   const double a1 = 2.0 * (k * k - 1.0) * norm, a2 = (1.0 - std::sqrt(2.0) * k + k * k) * norm;
   double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

   std::vector<double> output(Input.size());
   for (int i = 0; i < frames; i++) {
      double l = Input[size_t(i) * 2], r = Input[size_t(i) * 2 + 1];
      if (Settings.Swap) std::swap(l, r);
      const double m = (l + r) / 2, s = (l - r) / 2;
      const double high = b0 * s + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
      x2 = x1; x1 = s; y2 = y1; y1 = high;
      const double side = Settings.Split ? (b * (s - high) + w * high) : w * s;
      output[size_t(i) * 2] = t * gl * (m + side);
      output[size_t(i) * 2 + 1] = t * gr * (m - side);
   }
   return output;
}

//********************************************************************************************************************

static void test_configuration(AudioTestContext &Test)
{
   StereoFixture low(7999, true, StereoSettings());
   AUDIO_CHECK(low.Prepared IS ERR::NoSupport);
   StereoFixture high(192001, true, StereoSettings());
   AUDIO_CHECK(high.Prepared IS ERR::NoSupport);

   StereoFixture slow(8000, false, StereoSettings()), fast(192000, true, StereoSettings());
   AUDIO_CHECK(slow.Prepared IS ERR::Okay and fast.Prepared IS ERR::Okay);
   AUDIO_CHECK(slow.Processor.latency() IS 0 and fast.Processor.latency() IS 0);
   AUDIO_CHECK(slow.Processor.tail() IS AudioTail::FINITE);

   // Without split there is no tail at all.
   AUDIO_CHECK(fast.Processor.pending() IS false and fast.Processor.tail_frames() IS 0);
   AUDIO_CHECK(fast.Processor.decay_estimate() IS 0);

   // A zero rate leaves the processor inactive: audio passes untouched.
   StereoFixture idle(0, true, settings(0, 50, false, false, 150, 0, -12));
   AUDIO_CHECK(idle.Prepared IS ERR::Okay);
   std::vector<float> buffer = { 0.25f, -0.5f, 0.75f, 0.125f };
   const auto original = buffer;
   idle.run(buffer);
   AUDIO_CHECK(buffer IS original);
}

//********************************************************************************************************************
// The default settings, and equal widths with split, reproduce the input exactly.  Mono output only receives trim.

static void test_identity(AudioTestContext &Test)
{
   for (int rate : { 8000, 44100, 192000 }) {
      StereoFixture fixture(rate, true, StereoSettings());
      auto buffer = noise_buffer(4096, 2, 3);
      const auto original = buffer;
      fixture.run(buffer, 97);
      AUDIO_CHECK(buffer IS original);
   }

   // Equal widths with split are bit-identical to the unsplit result, at every crossover.

   for (double width : { 0.0, 100.0, 140.0, 200.0 }) {
      for (double crossover : { 40.0, 150.0, 1000.0 }) {
         StereoFixture plain(48000, true, settings(width, 30, true));
         StereoFixture split(48000, true, settings(width, 30, true, true, crossover, width));
         auto a = noise_buffer(4096, 2, 5), b = a;
         plain.run(a);
         split.run(b);
         AUDIO_CHECK(a IS b);
         AUDIO_CHECK(split.Processor.pending() IS false and split.Processor.tail_frames() IS 0);
      }
   }

   // Mono: width, balance, swap and split are no-ops; trim is applied.

   StereoFixture mono(44100, false, settings(0, -100, true, true, 40, 200, -6));
   auto buffer = noise_buffer(2048, 1, 9);
   const auto original = buffer;
   mono.run(buffer);
   const double trim = std::pow(10.0, -6.0 / 20.0);
   double error = 0;
   for (size_t i = 0; i < buffer.size(); i++) {
      error = std::max(error, std::abs(double(buffer[i]) - double(float(double(original[i]) * trim))));
   }
   AUDIO_CHECK(error IS 0);
   AUDIO_CHECK(mono.Processor.pending() IS false and mono.Processor.tail_frames() IS 0);
}

//********************************************************************************************************************
// Width, swap, balance and trim against the reference model, and their documented special cases.

static void test_equations(AudioTestContext &Test)
{
   const StereoSettings cases[] = {
      settings(0), settings(50), settings(150), settings(200), settings(100, -100), settings(100, -37.5),
      settings(100, 62.5), settings(100, 100), settings(100, 0, true), settings(170, -20, true, false, 150, 0, 12),
      settings(30, 45, false, false, 150, 0, -24)
   };

   double worst = 0;
   for (const auto &config : cases) {
      for (int rate : { 8000, 44100, 96000 }) {
         StereoFixture fixture(rate, true, config);
         auto buffer = noise_buffer(2048, 2, 11);
         const auto expected = reference(buffer, config, rate);
         fixture.run(buffer, 61);
         worst = std::max(worst, max_error(buffer, expected));
      }
   }
   AUDIO_CHECK(worst < 1e-6);

   // Zero width gives identical channels at the mid level; swap at full width exchanges channels exactly.

   StereoFixture mono_width(48000, true, settings(0));
   auto buffer = noise_buffer(1024, 2, 13);
   auto input = buffer;
   mono_width.run(buffer);
   bool identical = true, mid = true;
   for (size_t i = 0; i < buffer.size(); i += 2) {
      identical = identical and (buffer[i] IS buffer[i + 1]);
      mid = mid and (buffer[i] IS float((double(input[i]) + double(input[i + 1])) * 0.5));
   }
   AUDIO_CHECK(identical and mid);

   StereoFixture swapped(48000, true, settings(100, 0, true));
   buffer = input;
   swapped.run(buffer);
   bool exchanged = true;
   for (size_t i = 0; i < buffer.size(); i += 2) {
      exchanged = exchanged and (buffer[i] IS input[i + 1]) and (buffer[i + 1] IS input[i]);
   }
   AUDIO_CHECK(exchanged);

   // A single-channel signal peaks 50% higher at the maximum width; balance at either end silences a channel.

   StereoFixture wide(48000, true, settings(200));
   std::vector<float> one_sided = { 1.0f, 0.0f };
   wide.run(one_sided);
   AUDIO_CHECK(one_sided[0] IS 1.5f and one_sided[1] IS -0.5f);

   StereoFixture hard_left(48000, true, settings(100, -100));
   std::vector<float> pair = { 0.5f, 0.25f };
   hard_left.run(pair);
   AUDIO_CHECK(pair[0] IS 0.5f and pair[1] IS 0.0f);
}

//********************************************************************************************************************
// Split processing: the reference model, mono bass, the crossover point and an untouched mid signal.

static void test_split(AudioTestContext &Test)
{
   double worst = 0;
   for (int rate : { 8000, 44100, 96000, 192000 }) {
      for (double crossover : { 40.0, 150.0, 1000.0 }) {
         for (double bass : { 0.0, 100.0, 200.0 }) {
            const auto config = settings(150, 20, true, true, crossover, bass, -3);
            StereoFixture fixture(rate, true, config);
            auto buffer = noise_buffer(4096, 2, 17);
            const auto expected = reference(buffer, config, rate);
            fixture.run(buffer, 89);
            worst = std::max(worst, max_error(buffer, expected));
         }
      }
   }
   AUDIO_CHECK(worst < 1e-6);

   // Mono bass: with bass_width 0 the remaining side level is the bilinear Butterworth high-pass magnitude,
   // W^2 / sqrt(1 + W^4) with W = tan(pi f / fs) / tan(pi fc / fs).  It never exceeds unity, so there is no bump at
   // the crossover, and it falls by 12 dB per octave below it.

   const int rate = 48000, frames = rate;
   const auto config = settings(100, 0, false, true, 150, 0);
   kt::Log log("AudioTests");
   double worst_level = 0;
   for (double frequency : { 20.0, 30.0, 75.0, 150.0, 300.0, 1000.0, 5000.0 }) {
      StereoFixture fixture(rate, true, config);
      auto buffer = mid_side_tones(frames, rate, 0, 0, frequency, 0.5);
      fixture.run(buffer);
      const double ratio = side_rms(buffer, frames / 2, frames) / (0.5 / std::sqrt(2.0));
      const double warped = std::tan(std::numbers::pi * frequency / rate) / std::tan(std::numbers::pi * 150.0 / rate);
      const double expected = warped * warped / std::sqrt(1.0 + warped * warped * warped * warped);
      worst_level = std::max(worst_level, std::abs(20.0 * std::log10(ratio / expected)));
      AUDIO_CHECK(ratio <= 1.0 + 1e-3);
      log.msg("Stereo mono bass at 150 Hz crossover: %.0f Hz side %.1f dB", frequency, 20.0 * std::log10(ratio));
   }
   AUDIO_CHECK(worst_level < 0.05);

   // The mid signal is never filtered: in-phase content passes bit-identically whatever the split settings.

   StereoFixture mid_only(rate, true, settings(200, 0, false, true, 40, 0));
   auto buffer = mid_side_tones(4096, rate, 60, 0.5, 0, 0);
   const auto original = buffer;
   mid_only.run(buffer);
   AUDIO_CHECK(buffer IS original);
   AUDIO_CHECK(mid_only.Processor.history_live() IS false);
}

//********************************************************************************************************************
// Every gain ramps over exactly 10 ms; swap passes through zero; crossover edits crossfade and coalesce.

static void test_ramps(AudioTestContext &Test)
{
   const int rate = 48000, ramp = rate / 100;
   StereoFixture fixture(rate, true, StereoSettings());
   auto &p = fixture.Processor;

   fixture.change(settings(200, -50, true, true, 150, 0, 12));
   std::vector<float> buffer(size_t(ramp - 1) * 2, 0.1f);
   fixture.run(buffer);
   AUDIO_CHECK(p.side_gain() > 0 and p.side_gain() < 0.01 and p.bass_delta() > 1.99 and p.bass_delta() < 2.0);
   AUDIO_CHECK(p.swap_sign() < -0.99 and p.swap_sign() > -1.0);
   buffer.assign(2, 0.1f);
   fixture.run(buffer);
   AUDIO_CHECK(p.side_gain() IS 0.0 and p.swap_sign() IS -1.0 and p.bass_delta() IS 2.0);
   AUDIO_CHECK(p.left_gain() IS 1.0 and p.right_gain() IS 0.5 and p.trim_gain() IS std::pow(10.0, 12.0 / 20.0));

   // The swap sign passes through zero at the midpoint of its ramp.

   fixture.change(settings(200, -50, false, true, 150, 0, 12));
   buffer.assign(size_t(ramp / 2) * 2, 0.1f);
   fixture.run(buffer);
   AUDIO_CHECK(std::abs(p.swap_sign()) < 1e-9);

   // A crossover edit crossfades for 10 ms; a second edit during the fade is queued, and later edits coalesce.

   StereoFixture filters(rate, true, settings(100, 0, false, true, 150, 0));
   auto &q = filters.Processor;
   const uint64_t base_tail = q.tail_frames();
   filters.change(settings(100, 0, false, true, 40, 0));
   AUDIO_CHECK(q.fading() and (not q.fade_queued()));
   AUDIO_CHECK(q.tail_frames() > base_tail);
   buffer.assign(size_t(ramp / 2) * 2, 0.1f);
   filters.run(buffer);
   filters.change(settings(100, 0, false, true, 500, 0));
   filters.change(settings(100, 0, false, true, 1000, 0));
   AUDIO_CHECK(q.fading() and q.fade_queued());
   buffer.assign(size_t(ramp - ramp / 2) * 2, 0.1f);
   filters.run(buffer);
   AUDIO_CHECK(q.fading() and (not q.fade_queued()));
   const auto expected = stereo_filter(1000, rate);
   AUDIO_CHECK(q.current_filter().B0 IS expected.B0 and q.current_filter().A1 IS expected.A1);
   buffer.assign(size_t(ramp) * 2, 0.1f);
   filters.run(buffer);
   AUDIO_CHECK(not q.fading());

   // The crossfade against a model: from the edit, the old filter continues while the new filter starts from a copy
   // of its history, and the output blends linearly from old to new over 10 ms.  A pure side input with bass_width 0
   // and width 100 makes the left output HP(S) exactly.

   {
      StereoFixture model_fixture(rate, true, settings(100, 0, false, true, 1000, 0));
      const int edit = 1000, frames = 3000;
      auto input = noise_buffer(frames, 2, 43);
      for (size_t i = 0; i < input.size(); i += 2) input[i + 1] = -input[i];
      auto output = input;
      std::vector<float> first(output.begin(), output.begin() + edit * 2);
      std::vector<float> second(output.begin() + edit * 2, output.end());
      model_fixture.run(first);
      model_fixture.change(settings(100, 0, false, true, 40, 0));
      model_fixture.run(second, 77);

      const auto old_filter = stereo_filter(1000, rate), new_filter = stereo_filter(40, rate);
      StereoProcessor::History old_history, new_history;
      double worst_fade = 0;
      for (int i = 0; i < frames; i++) {
         const double s = (double(input[size_t(i) * 2]) - double(input[size_t(i) * 2 + 1])) * 0.5;
         if (i IS edit) new_history = old_history;
         double expected;
         if (i < edit) expected = old_history.run(old_filter, s);
         else {
            const double from = old_history.run(old_filter, s), to = new_history.run(new_filter, s);
            const double blend = std::min(1.0, double(i - edit) / double(ramp));
            expected = from + (to - from) * blend;
         }
         const float actual = (i < edit) ? first[size_t(i) * 2] : second[size_t(i - edit) * 2];
         worst_fade = std::max(worst_fade, std::abs(double(actual) - expected));
      }
      AUDIO_CHECK(worst_fade < 1e-6);
   }

   // Width and crossover edits on a playing low-frequency signal stay continuous: the largest step between frames is
   // close to that of the signal itself.  History is retained, so the filter does not restart.

   StereoFixture smooth(rate, true, settings(100, 0, false, true, 150, 0));
   auto tones = mid_side_tones(rate / 2, rate, 50, 0.3, 80, 0.3);
   double signal_step = 0;
   for (size_t i = 2; i < tones.size(); i += 2) {
      signal_step = std::max(signal_step, std::abs(double(tones[i]) - double(tones[i - 2])));
   }
   double worst_step = 0;
   float last = 0;
   for (int block = 0; block < 50; block++) {
      if (block % 5 IS 2) smooth.change(settings(block % 2 ? 200 : 0, 0, block % 3 IS 0, true, 40 + block * 15, 200));
      std::vector<float> part(tones.begin() + block * 960, tones.begin() + (block + 1) * 960);
      smooth.run(part);
      for (size_t i = 0; i < part.size(); i += 2) {
         if (block or i) worst_step = std::max(worst_step, std::abs(double(part[i]) - double(last)));
         last = part[i];
      }
   }
   AUDIO_CHECK(worst_step < signal_step * 3);
}

//********************************************************************************************************************
// The tail: only present with split and unequal widths, short, within the bound, and discarded output is below
// -120 dB of the peak side input.

static void test_tail(AudioTestContext &Test)
{
   kt::Log log("AudioTests");

   // The tail bound assumes that the high-pass impulse response has an L1 norm below 2.5.

   double worst_norm = 0;
   for (int rate : { 8000, 44100, 192000 }) {
      for (double crossover : { 40.0, 1000.0 }) {
         const auto f = stereo_filter(crossover, rate);
         StereoProcessor::History history;
         double norm = 0;
         for (int i = 0; i < rate * 2; i++) norm += std::abs(history.run(f, i ? 0.0 : 1.0));
         worst_norm = std::max(worst_norm, norm);
      }
   }
   AUDIO_CHECK(worst_norm < 2.5);
   log.msg("Stereo high-pass impulse response L1 norm: worst %.3f", worst_norm);

   for (int rate : { 8000, 44100, 192000 }) {
      for (double crossover : { 40.0, 150.0, 1000.0 }) {
         const auto config = settings(100, 0, false, true, crossover, 0);
         StereoFixture fixture(rate, true, config);
         AUDIO_CHECK(fixture.Processor.decay_estimate() > 0);

         // A full-scale side noise burst followed by silence.

         auto burst = noise_buffer(rate / 10, 2, 23, 1.0);
         for (size_t i = 0; i < burst.size(); i += 2) burst[i + 1] = -burst[i];
         fixture.run(burst);
         AUDIO_CHECK(fixture.Processor.pending());
         const auto bound = fixture.Processor.tail_frames();
         const int frames = fixture.drain(int(bound) * 2);
         AUDIO_CHECK(not fixture.Processor.pending());
         AUDIO_CHECK(uint64_t(frames) <= bound);
         if ((crossover IS 40.0) or (crossover IS 150.0)) {
            log.msg("Stereo split tail at %d Hz, %.0f Hz crossover: %.1f ms pending, bound %.1f ms, decay estimate "
               "%.1f ms", rate, crossover, frames * 1000.0 / rate, double(bound) * 1000.0 / rate,
               double(fixture.Processor.decay_estimate()) * 1000.0 / rate);
         }

         // Compare with a model that never discards history: the output that pending() gave up is inaudible.

         std::vector<float> input = noise_buffer(rate / 10, 2, 23, 1.0);
         for (size_t i = 0; i < input.size(); i += 2) input[i + 1] = -input[i];
         input.resize(input.size() + size_t(rate) * 2, 0.0f);
         const auto expected = reference(input, config, rate);
         StereoFixture compare(rate, true, config);
         compare.run(input, 128);
         AUDIO_CHECK(max_error(input, expected) < 1e-6);
      }
   }

   // An impulse at the lowest crossover and highest rate, the slowest decay that is supported.

   StereoFixture slow(192000, true, settings(0, 0, false, true, 40, 200));
   std::vector<float> impulse = { 1.0f, -1.0f };
   slow.run(impulse);
   const int frames = slow.drain(1 << 20);
   AUDIO_CHECK(uint64_t(frames) <= slow.Processor.tail_frames());
   AUDIO_CHECK(frames > 0 and frames < 192000 / 5);

   // Disabling split ends the tail once the delta has ramped to zero.

   StereoFixture toggle(48000, true, settings(100, 0, false, true, 150, 0));
   auto burst = noise_buffer(4800, 2, 29);
   toggle.run(burst);
   AUDIO_CHECK(toggle.Processor.pending());
   toggle.change(settings(100));
   AUDIO_CHECK(toggle.Processor.pending()); // Still ramping out
   std::vector<float> silence(size_t(480) * 2, 0.0f);
   toggle.run(silence);
   AUDIO_CHECK(toggle.Processor.bass_delta() IS 0);
   AUDIO_CHECK((not toggle.Processor.pending()) and toggle.Processor.tail_frames() IS 0);

   // Mono content never excites the filter.

   StereoFixture centred(48000, true, settings(100, 0, false, true, 150, 0));
   auto mid = mid_side_tones(4800, 48000, 100, 0.5, 0, 0);
   centred.run(mid);
   AUDIO_CHECK(not centred.Processor.pending());
}

//********************************************************************************************************************
// Reset discards history and fades and applies the latest settings; updates while inactive are kept for reset.

static void test_lifecycle(AudioTestContext &Test)
{
   StereoFixture fixture(48000, true, settings(100, 0, false, true, 150, 0));
   auto &p = fixture.Processor;
   auto burst = noise_buffer(4800, 2, 31);
   fixture.run(burst);
   fixture.change(settings(50, 0, false, true, 300, 0));
   AUDIO_CHECK(p.pending() and p.fading());

   p.reset();
   AUDIO_CHECK((not p.pending()) and (not p.fading()) and (not p.history_live()));
   AUDIO_CHECK(p.side_gain() IS 0.0 and p.bass_delta() IS 0.5);

   // ResetPending: the update is stored and reset() applies it without a ramp.

   fixture.Effect.ResetPending = true;
   fixture.change(settings(180, 40, true));
   AUDIO_CHECK(p.side_gain() IS 0.0);
   fixture.Effect.ResetPending = false;
   p.reset();
   AUDIO_CHECK(p.side_gain() IS 1.8 and p.swap_sign() IS -1.0 and std::abs(p.left_gain() - 0.6) < 1e-15);

   // A rate change through prepare and publish.

   fixture.Effect.OutputRate = 96000;
   std::unique_ptr<AudioEffectConfiguration> config;
   AUDIO_CHECK(p.prepare(96000, true, config) IS ERR::Okay);
   config->publish();
   p.reset();
   AUDIO_CHECK(p.SampleRate IS 96000);
   const auto expected = stereo_filter(150, 96000);
   AUDIO_CHECK(p.current_filter().B0 IS expected.B0);

   // An update derived for another rate is ignored until reset.

   StereoTarget stale;
   const auto next = settings(20);
   stereo_target(next, 48000, stale);
   p.update(next, &stale);
   AUDIO_CHECK(p.side_gain() IS 1.8);
   p.reset();
   AUDIO_CHECK(p.side_gain() IS 0.2);

   // Non-finite input is silence and does not poison the filter history.

   StereoFixture hostile(48000, true, settings(100, 0, false, true, 150, 0));
   std::vector<float> bad = { std::numeric_limits<float>::quiet_NaN(), 0.5f,
      std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 0.25f, -0.25f };
   hostile.run(bad);
   bool finite = true;
   for (auto sample : bad) finite = finite and std::isfinite(sample);
   AUDIO_CHECK(finite);
   AUDIO_CHECK(hostile.drain(48000) < 48000);
}

//********************************************************************************************************************
// Output does not depend on how the frames are divided into blocks, including edits at fixed frame positions.

static void test_blocks(AudioTestContext &Test)
{
   const int rate = 44100;
   const auto input = noise_buffer(8192, 2, 37);
   std::vector<std::vector<float>> results;
   for (int block : { 8192, 1024, 333, 64, 1 }) {
      StereoFixture fixture(rate, true, settings(100, 0, false, true, 150, 0));
      std::vector<float> output;
      for (int part = 0; part < 4; part++) {
         if (part IS 1) fixture.change(settings(170, -30, true, true, 600, 50, -6));
         if (part IS 2) fixture.change(settings(40, 20, false, true, 80, 180, 3));
         std::vector<float> chunk(input.begin() + part * 4096, input.begin() + (part + 1) * 4096);
         fixture.run(chunk, block);
         output.insert(output.end(), chunk.begin(), chunk.end());
      }
      results.push_back(std::move(output));
   }
   for (size_t i = 1; i < results.size(); i++) AUDIO_CHECK(results[i] IS results[0]);
}

//********************************************************************************************************************
// The worst-case render cost at 192 kHz stereo is logged.

static void test_resources(AudioTestContext &Test)
{
   StereoFixture fixture(192000, true, settings(150, 20, false, true, 40, 0));
   uint32_t seed = 41;
   std::vector<float> buffer(size_t(1024) * 2);
   double worst_block = 0;
   for (int block = 0; block < 400; block++) {
      fixture.change(settings(block % 200, (block % 7) * 10.0, block & 1, true, 40 + (block % 9) * 100.0, 200));
      for (auto &sample : buffer) sample = float(noise(seed));
      const auto start = std::chrono::steady_clock::now();
      fixture.Processor.process(buffer.data(), 1024);
      const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start);
      worst_block = std::max(worst_block, elapsed.count());
   }
   AUDIO_CHECK(peak(buffer) < 4.0);
   AUDIO_CHECK(sizeof(StereoProcessor) < 1024);
   kt::Log("AudioTests").msg("Stereo at 192 kHz stereo: worst 1024-frame block %.1f us, processor %d bytes",
      worst_block, int(sizeof(StereoProcessor)));
}

//********************************************************************************************************************

static void run(AudioTestContext &Test)
{
   test_configuration(Test);
   test_identity(Test);
   test_equations(Test);
   test_split(Test);
   test_ramps(Test);
   test_tail(Test);
   test_lifecycle(Test);
   test_blocks(Test);
   test_resources(Test);
}

} // namespace audio_tests_audio_stereo_dsp
