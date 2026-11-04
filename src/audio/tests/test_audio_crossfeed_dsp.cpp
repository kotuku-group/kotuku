// Included by audio.cpp to exercise the module implementation.

#include <chrono>
#include <complex>

namespace audio_tests_audio_crossfeed_dsp {

// Configure exactly as activation does: prepare outside the lock, publish, then reset on the render thread.

struct CrossfeedFixture {
   extAudioEffect Effect{nullptr, 1};
   CrossfeedProcessor Processor;
   ERR Prepared;
   int Channels;

   CrossfeedFixture(int Rate, bool Stereo, const CrossfeedSettings &Settings) : Processor(&Effect, Settings) {
      Effect.OutputRate = Rate;
      Effect.Stereo = Stereo;
      Effect.ResetPending = false;
      Channels = Stereo ? 2 : 1;
      std::unique_ptr<AudioEffectConfiguration> config;
      Prepared = Processor.prepare(Rate, Stereo, config);
      if (config) config->publish();
      Processor.reset();
   }

   // Parameter updates derive their target on the control thread, as CrossfeedUpdate does.
   void change(const CrossfeedSettings &Settings) {
      CrossfeedTarget target;
      const bool valid = crossfeed_target(Settings, Processor.SampleRate, target);
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

static CrossfeedSettings settings(double Amount, double Cutoff = 700, double Delay = 0.3, double Gain = 0)
{
   return CrossfeedSettings { .Amount = Amount, .Cutoff = Cutoff, .Delay = Delay, .Gain = Gain };
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

// Interleaved stereo tone with independent levels per channel.

static std::vector<float> tone(int Frames, int Rate, double Frequency, double Left, double Right)
{
   std::vector<float> buffer(size_t(Frames) * 2);
   for (int i = 0; i < Frames; i++) {
      const double s = std::sin(2.0 * std::numbers::pi * Frequency * i / Rate);
      buffer[size_t(i) * 2] = float(Left * s);
      buffer[size_t(i) * 2 + 1] = float(Right * s);
   }
   return buffer;
}

static double peak(const std::vector<float> &Buffer, size_t Start = 0)
{
   double result = 0;
   for (size_t i = Start; i < Buffer.size(); i++) result = std::max(result, std::abs(double(Buffer[i])));
   return result;
}

// RMS of one channel of an interleaved stereo buffer, from frame Start to End.

static double channel_rms(const std::vector<float> &Buffer, int Channel, size_t Start, size_t End)
{
   double sum = 0;
   for (size_t i = Start; i < End; i++) sum += double(Buffer[i * 2 + Channel]) * double(Buffer[i * 2 + Channel]);
   return std::sqrt(sum / double(End - Start));
}

static double max_error(const std::vector<float> &Actual, const std::vector<double> &Expected)
{
   double result = 0;
   for (size_t i = 0; i < Actual.size(); i++) result = std::max(result, std::abs(double(Actual[i]) - Expected[i]));
   return result;
}

//********************************************************************************************************************
// An independent model of the published equations for fixed settings.  The low-pass uses the textbook bilinear form
// b0 = K / (1 + K), a1 = (K - 1) / (K + 1) rather than the processor's pole form, the filtered signals are kept in full
// so that the delay is an explicit interpolation between two past values, and the history is never discarded.

static std::vector<double> reference(const std::vector<float> &Input, const CrossfeedSettings &Settings, int Rate)
{
   const size_t frames = Input.size() / 2;
   const double a = Settings.Amount / 100.0, t = std::pow(10.0, Settings.Gain / 20.0);
   const double k = std::tan(std::numbers::pi * Settings.Cutoff / Rate);
   const double b0 = k / (1.0 + k), a1 = (k - 1.0) / (k + 1.0);
   const double d = Settings.Delay * Rate / 1000.0;
   const int whole = int(std::floor(d));
   const double fraction = d - whole;

   std::vector<double> filtered[2] = { std::vector<double>(frames), std::vector<double>(frames) };
   for (int c = 0; c < 2; c++) {
      double x1 = 0, y1 = 0;
      for (size_t i = 0; i < frames; i++) {
         const double x = Input[i * 2 + c];
         const double y = b0 * x + b0 * x1 - a1 * y1;
         x1 = x; y1 = y;
         filtered[c][i] = y;
      }
   }

   auto past = [&](int Channel, int64_t Index) { return (Index < 0) ? 0.0 : filtered[Channel][size_t(Index)]; };

   std::vector<double> output(Input.size());
   for (size_t i = 0; i < frames; i++) {
      const int64_t n = int64_t(i) - whole;
      const double from_right = (1.0 - fraction) * past(1, n) + fraction * past(1, n - 1);
      const double from_left = (1.0 - fraction) * past(0, n) + fraction * past(0, n - 1);
      output[i * 2] = t * (Input[i * 2] + a * from_right) / (1.0 + a);
      output[i * 2 + 1] = t * (Input[i * 2 + 1] + a * from_left) / (1.0 + a);
   }
   return output;
}

// The complex response of the crossfed term, filter times interpolated delay, at Frequency.

static std::complex<double> cross_response(const CrossfeedSettings &Settings, int Rate, double Frequency)
{
   const double omega = 2.0 * std::numbers::pi * Frequency / Rate;
   const double k = std::tan(std::numbers::pi * Settings.Cutoff / Rate);
   const double b0 = k / (1.0 + k), a1 = (k - 1.0) / (k + 1.0);
   const auto z1 = std::polar(1.0, -omega);
   const auto filter = b0 * (1.0 + z1) / (1.0 + a1 * z1);
   const double d = Settings.Delay * Rate / 1000.0;
   const int whole = int(std::floor(d));
   const double fraction = d - whole;
   const auto delay = std::polar(1.0, -omega * whole) * ((1.0 - fraction) + fraction * z1);
   return filter * delay;
}

//********************************************************************************************************************

static void test_configuration(AudioTestContext &Test)
{
   CrossfeedFixture low(7999, true, CrossfeedSettings());
   AUDIO_CHECK(low.Prepared IS ERR::NoSupport);
   CrossfeedFixture high(192001, true, CrossfeedSettings());
   AUDIO_CHECK(high.Prepared IS ERR::NoSupport);

   CrossfeedFixture slow(8000, false, CrossfeedSettings()), fast(192000, true, CrossfeedSettings());
   AUDIO_CHECK(slow.Prepared IS ERR::Okay and fast.Prepared IS ERR::Okay);
   AUDIO_CHECK(slow.Processor.latency() IS 0 and fast.Processor.latency() IS 0);
   AUDIO_CHECK(slow.Processor.tail() IS AudioTail::FINITE);

   // Pending only once there is history; the tail bound is published while the crossfeed is audible.
   AUDIO_CHECK(fast.Processor.pending() IS false);
   AUDIO_CHECK(fast.Processor.tail_frames() > 0 and fast.Processor.decay_estimate() > 0);

   // Zero amount and mono output have no tail.
   CrossfeedFixture off(48000, true, settings(0));
   AUDIO_CHECK(off.Processor.tail_frames() IS 0 and off.Processor.decay_estimate() IS 0);
   AUDIO_CHECK(slow.Processor.tail_frames() IS 0);

   // A zero rate leaves the processor inactive: audio passes untouched.
   CrossfeedFixture idle(0, true, settings(50, 100, 1, -12));
   AUDIO_CHECK(idle.Prepared IS ERR::Okay);
   std::vector<float> buffer = { 0.25f, -0.5f, 0.75f, 0.125f };
   const auto original = buffer;
   idle.run(buffer);
   AUDIO_CHECK(buffer IS original);

   // The longest delay at the highest rate fits the ring, and the corner clamp does not engage within the range.
   AUDIO_CHECK(crossfeed_reach(CROSSFEED_MAX_RATE) < CROSSFEED_CAPACITY);
   AUDIO_CHECK(crossfeed_corner(2000, CROSSFEED_MIN_RATE) IS 2000.0);
   AUDIO_CHECK(crossfeed_corner(2000, 4000) IS 1800.0);
   AUDIO_CHECK(crossfeed_pole(2000, CROSSFEED_MIN_RATE) >= 0 and crossfeed_pole(100, CROSSFEED_MAX_RATE) < 1);
}

//********************************************************************************************************************
// Zero amount reproduces the input exactly; mono output only receives trim.

static void test_identity(AudioTestContext &Test)
{
   for (int rate : { 8000, 44100, 192000 }) {
      CrossfeedFixture fixture(rate, true, settings(0, 100, 1));
      auto buffer = noise_buffer(4096, 2, 3);
      const auto original = buffer;
      fixture.run(buffer, 97);
      AUDIO_CHECK(buffer IS original);
      AUDIO_CHECK(fixture.Processor.pending() IS false);
   }

   CrossfeedFixture mono(44100, false, settings(50, 100, 1, -6));
   auto buffer = noise_buffer(2048, 1, 9);
   const auto original = buffer;
   mono.run(buffer);
   const double trim = std::pow(10.0, -6.0 / 20.0);
   bool exact = true;
   for (size_t i = 0; i < buffer.size(); i++) exact = exact and (buffer[i] IS float(double(original[i]) * trim));
   AUDIO_CHECK(exact);
   AUDIO_CHECK(mono.Processor.pending() IS false and mono.Processor.tail_frames() IS 0);

   // Centred DC settles at its own level: the normalisation restores unity where the crossfeed is complete.

   CrossfeedFixture centred(48000, true, settings(50, 100, 1));
   std::vector<float> dc(size_t(48000) * 2, 0.5f);
   centred.run(dc);
   AUDIO_CHECK(std::abs(double(dc[dc.size() - 2]) - 0.5) < 1e-6 and std::abs(double(dc.back()) - 0.5) < 1e-6);
}

//********************************************************************************************************************
// The equations against the reference model at every supported rate class, including fractional delays.

static void test_equations(AudioTestContext &Test)
{
   const CrossfeedSettings cases[] = {
      settings(20), settings(50, 100, 0), settings(50, 2000, 1), settings(10, 300, 0.5, 6),
      settings(35, 650, 0.27, -12), settings(5, 1500, 0.0137, 12)
   };

   double worst = 0;
   for (const auto &config : cases) {
      for (int rate : { 8000, 44100, 96000, 192000 }) {
         CrossfeedFixture fixture(rate, true, config);
         auto buffer = noise_buffer(4096, 2, 11);
         const auto expected = reference(buffer, config, rate);
         fixture.run(buffer, 61);
         worst = std::max(worst, max_error(buffer, expected));
      }
   }
   AUDIO_CHECK(worst < 1e-6);

   // An impulse in the left channel reaches the right after the delay: with no delay on the same frame, with 1 ms at
   // 48 kHz exactly 48 frames later.

   for (double delay : { 0.0, 1.0 }) {
      CrossfeedFixture fixture(48000, true, settings(50, 2000, delay));
      std::vector<float> impulse(size_t(200) * 2, 0.0f);
      impulse[0] = 1.0f;
      fixture.run(impulse);
      const int expected_frame = int(delay * 48);
      int first = -1;
      for (int i = 0; i < 200; i++) {
         if (impulse[size_t(i) * 2 + 1] != 0) { first = i; break; }
      }
      AUDIO_CHECK(first IS expected_frame);
      AUDIO_CHECK(impulse[0] IS float(1.0 / 1.5));
   }
}

//********************************************************************************************************************
// Measured tone levels against the analytic response, for a hard-panned and a centred source.  The centred levels are
// logged for the documentation.

static void test_response(AudioTestContext &Test)
{
   kt::Log log("AudioTests");
   const int rate = 48000, frames = rate;
   double worst = 0;

   for (const auto &config : { CrossfeedSettings(), settings(50, 100, 1), settings(35, 2000, 0.0137) }) {
      const double a = config.Amount / 100.0;
      for (double frequency : { 50.0, 100.0, 300.0, 700.0, 2000.0, 5000.0, 12000.0 }) {
         // Left only: the left output is L / (1 + a), the right output the crossfed term.

         CrossfeedFixture panned(rate, true, config);
         auto buffer = tone(frames, rate, frequency, 0.5, 0);
         panned.run(buffer);
         const double input = 0.5 / std::sqrt(2.0);
         const double direct = channel_rms(buffer, 0, frames / 2, frames) / input;
         const double cross = channel_rms(buffer, 1, frames / 2, frames) / input;
         const double expected_cross = a / (1.0 + a) * std::abs(cross_response(config, rate, frequency));
         worst = std::max(worst, std::abs(20.0 * std::log10(direct * (1.0 + a))));
         worst = std::max(worst, std::abs(20.0 * std::log10(cross / expected_cross)));

         // Centred: both outputs are (1 + a H) / (1 + a) times the input.

         CrossfeedFixture centred(rate, true, config);
         buffer = tone(frames, rate, frequency, 0.5, 0.5);
         centred.run(buffer);
         const double level = channel_rms(buffer, 0, frames / 2, frames) / input;
         const double expected = std::abs(1.0 + a * cross_response(config, rate, frequency)) / (1.0 + a);
         worst = std::max(worst, std::abs(20.0 * std::log10(level / expected)));
         log.msg("Crossfeed %.0f%%, %.0f Hz, %.2f ms at %.0f Hz: centred %+.2f dB, opposite ear %+.2f dB",
            config.Amount, config.Cutoff, config.Delay, frequency, 20.0 * std::log10(level),
            20.0 * std::log10(cross));
      }
   }
   AUDIO_CHECK(worst < 0.05);
}

//********************************************************************************************************************
// Before trim, no output exceeds the peak input, including during edits.

static void test_peak_bound(AudioTestContext &Test)
{
   double worst = 0;
   for (int rate : { 8000, 44100, 192000 }) {
      CrossfeedFixture fixture(rate, true, settings(50, 2000, 0.5));
      uint32_t seed = 7;
      for (int block = 0; block < 100; block++) {
         if (block % 3 IS 0) {
            fixture.change(settings((block * 7) % 51, 100 + (block * 97) % 1901, (block % 11) / 10.0));
         }
         std::vector<float> buffer(size_t(256) * 2);
         // Full-scale noise and alternating hard-panned square bursts.
         for (size_t i = 0; i < buffer.size(); i++) {
            buffer[i] = (block & 1) ? float(noise(seed)) : float(((i / 64) & 1) ? ((i & 1) ? 1.0 : -1.0) : 0.0);
         }
         fixture.run(buffer);
         worst = std::max(worst, peak(buffer));
      }
   }
   AUDIO_CHECK(worst <= 1.0);
}

//********************************************************************************************************************
// Amount, pole and trim ramp over exactly 10 ms; delay edits crossfade between read heads and coalesce.

static void test_ramps(AudioTestContext &Test)
{
   const int rate = 48000, ramp = rate / 100;
   CrossfeedFixture fixture(rate, true, settings(0));
   auto &p = fixture.Processor;

   fixture.change(settings(50, 100, 0.3, 12));
   std::vector<float> buffer(size_t(ramp - 1) * 2, 0.1f);
   fixture.run(buffer);
   AUDIO_CHECK(p.amount_gain() > 0.49 and p.amount_gain() < 0.5);
   AUDIO_CHECK(p.filter_pole() > crossfeed_pole(700, rate) and p.filter_pole() < crossfeed_pole(100, rate));
   buffer.assign(2, 0.1f);
   fixture.run(buffer);
   AUDIO_CHECK(p.amount_gain() IS 0.5 and p.filter_pole() IS crossfeed_pole(100, rate));
   AUDIO_CHECK(p.trim_gain() IS std::pow(10.0, 12.0 / 20.0));

   // A delay edit crossfades for 10 ms; a second edit during the crossfade is queued, and later edits coalesce.

   CrossfeedFixture heads(rate, true, settings(20, 700, 0.3));
   auto &q = heads.Processor;
   heads.change(settings(20, 700, 1.0));
   AUDIO_CHECK(q.crossfading() and (not q.delay_queued()));
   buffer.assign(size_t(ramp / 2) * 2, 0.1f);
   heads.run(buffer);
   heads.change(settings(20, 700, 0.5));
   heads.change(settings(20, 700, 0.0));
   AUDIO_CHECK(q.crossfading() and q.delay_queued() and q.read_distance() IS 48.0);
   buffer.assign(size_t(ramp - ramp / 2 + 1) * 2, 0.1f);
   heads.run(buffer);
   AUDIO_CHECK(q.crossfading() and (not q.delay_queued()) and q.read_distance() IS 0.0);
   buffer.assign(size_t(ramp) * 2, 0.1f);
   heads.run(buffer);
   AUDIO_CHECK(not q.crossfading());

   // The read-head crossfade against a model.  Each output is linear in its crossfed term, so blending the two heads
   // linearly over 10 ms from the edit blends the reference outputs for the old and new delays in the same way.

   {
      const auto before = settings(50, 700, 0.1), after = settings(50, 700, 0.9);
      CrossfeedFixture model_fixture(rate, true, before);
      const int edit = 1000, frames = 3000;
      auto input = noise_buffer(frames, 2, 43);
      std::vector<float> first(input.begin(), input.begin() + edit * 2);
      std::vector<float> second(input.begin() + edit * 2, input.end());
      model_fixture.run(first);
      model_fixture.change(after);
      model_fixture.run(second, 77);

      const auto old_head = reference(input, before, rate), new_head = reference(input, after, rate);
      double worst_fade = 0;
      for (int i = 0; i < frames; i++) {
         for (int c = 0; c < 2; c++) {
            const size_t index = size_t(i) * 2 + c;
            double expected = old_head[index];
            if (i >= edit) {
               const double blend = std::min(1.0, double(i - edit) / double(ramp));
               expected += (new_head[index] - expected) * blend;
            }
            const float actual = (i < edit) ? first[index] : second[index - size_t(edit) * 2];
            worst_fade = std::max(worst_fade, std::abs(double(actual) - expected));
         }
      }
      AUDIO_CHECK(worst_fade < 1e-6);
   }

   // Edits of every parameter on a playing signal stay continuous: the largest step between frames is close to that
   // of the signal itself.

   CrossfeedFixture smooth(rate, true, CrossfeedSettings());
   auto tones = tone(rate / 2, rate, 80, 0.6, -0.2);
   double signal_step = 0;
   for (size_t i = 2; i < tones.size(); i += 2) {
      signal_step = std::max(signal_step, std::abs(double(tones[i + 1]) - double(tones[i - 1])));
   }
   double worst_step = 0;
   float last = 0;
   for (int block = 0; block < 25; block++) {
      if (block % 5 IS 2) smooth.change(settings(block % 2 ? 50 : 0, 100 + block * 70, (block % 3) * 0.5));
      std::vector<float> part(tones.begin() + block * 1920, tones.begin() + (block + 1) * 1920);
      smooth.run(part);
      for (size_t i = 0; i < part.size(); i += 2) {
         if (block or i) worst_step = std::max(worst_step, std::abs(double(part[i + 1]) - double(last)));
         last = part[i + 1];
      }
   }
   AUDIO_CHECK(worst_step < signal_step * 2);
}

//********************************************************************************************************************
// The tail: short, within the bound, and the output given up on retirement is inaudible.

static void test_tail(AudioTestContext &Test)
{
   kt::Log log("AudioTests");

   for (int rate : { 8000, 44100, 192000 }) {
      for (double cutoff : { 100.0, 700.0, 2000.0 }) {
         const auto config = settings(50, cutoff, 1);
         CrossfeedFixture fixture(rate, true, config);

         auto burst = noise_buffer(rate / 10, 2, 23, 1.0);
         fixture.run(burst);
         AUDIO_CHECK(fixture.Processor.pending());
         const auto bound = fixture.Processor.tail_frames();
         const int frames = fixture.drain(int(bound) * 2);
         AUDIO_CHECK(not fixture.Processor.pending());
         AUDIO_CHECK(uint64_t(frames) <= bound);
         log.msg("Crossfeed tail at %d Hz, %.0f Hz cutoff, 1 ms delay: %.1f ms pending, bound %.1f ms, decay "
            "estimate %.1f ms", rate, cutoff, frames * 1000.0 / rate, double(bound) * 1000.0 / rate,
            double(fixture.Processor.decay_estimate()) * 1000.0 / rate);

         // Compare with a model that never discards history.

         auto input = noise_buffer(rate / 10, 2, 23, 1.0);
         input.resize(input.size() + size_t(rate) * 2, 0.0f);
         const auto expected = reference(input, config, rate);
         CrossfeedFixture compare(rate, true, config);
         compare.run(input, 128);
         AUDIO_CHECK(max_error(input, expected) < 1e-6);
         AUDIO_CHECK(not compare.Processor.pending());
      }
   }

   // Reducing the amount to zero ends the tail once it has ramped out.

   CrossfeedFixture toggle(48000, true, CrossfeedSettings());
   auto burst = noise_buffer(4800, 2, 29);
   toggle.run(burst);
   AUDIO_CHECK(toggle.Processor.pending());
   toggle.change(settings(0));
   AUDIO_CHECK(toggle.Processor.pending()); // Still ramping out
   std::vector<float> silence(size_t(480) * 2, 0.0f);
   toggle.run(silence);
   AUDIO_CHECK(toggle.Processor.amount_gain() IS 0);
   AUDIO_CHECK((not toggle.Processor.pending()) and toggle.Processor.tail_frames() IS 0);

   // A silent stream never becomes pending.

   CrossfeedFixture quiet(48000, true, CrossfeedSettings());
   std::vector<float> zeros(size_t(4800) * 2, 0.0f);
   quiet.run(zeros);
   AUDIO_CHECK((not quiet.Processor.pending()) and (not quiet.Processor.history_live()));
}

//********************************************************************************************************************
// Reset discards history and crossfades and applies the latest settings; updates while inactive are kept for reset.

static void test_lifecycle(AudioTestContext &Test)
{
   CrossfeedFixture fixture(48000, true, CrossfeedSettings());
   auto &p = fixture.Processor;
   auto burst = noise_buffer(4800, 2, 31);
   fixture.run(burst);
   fixture.change(settings(40, 300, 0.8));
   AUDIO_CHECK(p.pending() and p.crossfading());

   p.reset();
   AUDIO_CHECK((not p.pending()) and (not p.crossfading()) and (not p.history_live()));
   AUDIO_CHECK(p.amount_gain() IS 0.4 and p.read_distance() IS 0.8 * 48000.0 / 1000.0);

   // ResetPending: the update is stored and reset() applies it without a ramp.

   fixture.Effect.ResetPending = true;
   fixture.change(settings(10, 1000, 0));
   AUDIO_CHECK(p.amount_gain() IS 0.4);
   fixture.Effect.ResetPending = false;
   p.reset();
   AUDIO_CHECK(p.amount_gain() IS 0.1 and p.read_distance() IS 0.0);

   // A rate change through prepare and publish.

   fixture.Effect.OutputRate = 96000;
   std::unique_ptr<AudioEffectConfiguration> config;
   AUDIO_CHECK(p.prepare(96000, true, config) IS ERR::Okay);
   config->publish();
   p.reset();
   AUDIO_CHECK(p.SampleRate IS 96000 and p.filter_pole() IS crossfeed_pole(1000, 96000));

   // An update derived for another rate is ignored until reset.

   CrossfeedTarget stale;
   const auto next = settings(25);
   crossfeed_target(next, 48000, stale);
   p.update(next, &stale);
   AUDIO_CHECK(p.amount_gain() IS 0.1);
   p.reset();
   AUDIO_CHECK(p.amount_gain() IS 0.25 and p.read_distance() IS 0.3 * 96000.0 / 1000.0);

   // Non-finite input is silence and does not poison the history.

   CrossfeedFixture hostile(48000, true, settings(50));
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
      CrossfeedFixture fixture(rate, true, CrossfeedSettings());
      std::vector<float> output;
      for (int part = 0; part < 4; part++) {
         if (part IS 1) fixture.change(settings(45, 150, 0.9, -6));
         if (part IS 2) fixture.change(settings(5, 1800, 0.05, 3));
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
   CrossfeedFixture fixture(192000, true, settings(50, 100, 1));
   uint32_t seed = 41;
   std::vector<float> buffer(size_t(1024) * 2);
   double worst_block = 0;
   for (int block = 0; block < 400; block++) {
      fixture.change(settings(block % 51, 100 + (block % 19) * 100.0, (block % 11) / 10.0, (block % 5) - 2.0));
      for (auto &sample : buffer) sample = float(noise(seed));
      const auto start = std::chrono::steady_clock::now();
      fixture.Processor.process(buffer.data(), 1024);
      const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start);
      worst_block = std::max(worst_block, elapsed.count());
   }
   AUDIO_CHECK(peak(buffer) < 2.0);
   AUDIO_CHECK(sizeof(CrossfeedProcessor) < 8192);
   kt::Log("AudioTests").msg("Crossfeed at 192 kHz stereo: worst 1024-frame block %.1f us, processor %d bytes",
      worst_block, int(sizeof(CrossfeedProcessor)));
}

//********************************************************************************************************************

static void run(AudioTestContext &Test)
{
   test_configuration(Test);
   test_identity(Test);
   test_equations(Test);
   test_response(Test);
   test_peak_bound(Test);
   test_ramps(Test);
   test_tail(Test);
   test_lifecycle(Test);
   test_blocks(Test);
   test_resources(Test);
}

} // namespace audio_tests_audio_crossfeed_dsp
