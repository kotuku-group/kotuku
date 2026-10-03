// Included by audio.cpp to exercise the module implementation.

#include <chrono>

namespace audio_tests_audio_delay_dsp {

// Configure exactly as activation does: prepare outside the lock, publish, then reset on the render thread.

struct DelayFixture {
   extAudioEffect Effect{nullptr, 1};
   DelayProcessor Processor;
   ERR Prepared;
   int Channels;

   DelayFixture(int Rate, bool Stereo, const DelaySettings &Settings) : Processor(&Effect, Settings) {
      Effect.OutputRate = Rate;
      Effect.Stereo = Stereo;
      Effect.ResetPending = false;
      Channels = Stereo ? 2 : 1;
      std::unique_ptr<AudioEffectConfiguration> config;
      Prepared = Processor.prepare(Rate, Stereo, config);
      if (config) config->publish();
      Processor.reset();
   }

   // Parameter updates derive their target on the control thread, as DelayUpdate does.
   void change(const DelaySettings &Settings) {
      DelayTarget target;
      const bool valid = delay_target(Settings, Processor.Rate, target);
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

   // Process silence one frame at a time until the processor is idle or Limit frames have passed.
   uint64_t drain(uint64_t Limit) {
      uint64_t frames = 0;
      float silence[2];
      while (Processor.pending() and (frames < Limit)) {
         silence[0] = silence[1] = 0;
         Processor.process(silence, 1);
         frames++;
      }
      return frames;
   }
};

static DelaySettings wet_settings(double Time, double Feedback, double Damping = 0)
{
   return DelaySettings { .Time = Time, .Feedback = Feedback, .Damping = Damping, .Mode = DELAY_NORMAL, .Mix = 100 };
}

static double noise(uint32_t &Seed)
{
   Seed = Seed * 1664525u + 1013904223u;
   return double(Seed >> 8) / double(1u << 23) - 1.0;
}

//********************************************************************************************************************
// An independent model of the published equations, using unbounded double-precision history instead of a ring.

static std::vector<double> reference(const std::vector<float> &Input, int Channels, const DelaySettings &Settings,
   int Rate)
{
   const int frames = int(Input.size()) / Channels;
   const double delay = Settings.Time * double(Rate) / 1000.0;
   const double g = Settings.Feedback / 100.0, m = Settings.Mix / 100.0;
   double a, b;
   delay_damping(Settings.Damping, Rate, a, b);
   const bool ping_pong = (Channels IS 2) and (Settings.Mode IS DELAY_PING_PONG);
   const int whole = int(std::floor(delay));
   const double fraction = delay - double(whole);

   std::vector<double> history(Input.size(), 0.0), output(Input.size(), 0.0);
   auto past = [&](int Frame, int Channel) { return Frame < 0 ? 0.0 : history[size_t(Frame) * Channels + Channel]; };
   double state[2] = { 0, 0 };
   for (int n = 0; n < frames; n++) {
      double x[2], d[2], fb[2];
      for (int c = 0; c < Channels; c++) {
         x[c] = Input[size_t(n) * Channels + c];
         d[c] = past(n - whole, c) * (1.0 - fraction) + past(n - whole - 1, c) * fraction;
         state[c] += a * (d[c] - state[c]);
         fb[c] = (1.0 - b) * d[c] + b * state[c];
         output[size_t(n) * Channels + c] = (1.0 - m) * x[c] + m * d[c];
      }
      if (ping_pong) {
         history[size_t(n) * 2] = 0.5 * (x[0] + x[1]) + g * fb[1];
         history[size_t(n) * 2 + 1] = g * fb[0];
      }
      else for (int c = 0; c < Channels; c++) history[size_t(n) * Channels + c] = x[c] + g * fb[c];
   }
   return output;
}

static double max_error(const std::vector<float> &Actual, const std::vector<double> &Expected)
{
   double error = 0;
   for (size_t i = 0; i < Actual.size(); i++) error = std::max(error, std::abs(double(Actual[i]) - Expected[i]));
   return error;
}

//********************************************************************************************************************

static void test_configuration(AudioTestContext &Test)
{
   DelayFixture low(4000, false, DelaySettings());
   AUDIO_CHECK(low.Prepared IS ERR::NoSupport);
   DelayFixture high(384000, true, DelaySettings());
   AUDIO_CHECK(high.Prepared IS ERR::NoSupport);

   // Supported endpoints allocate a ring covering two seconds plus interpolation support.
   DelayFixture slow(8000, false, DelaySettings()), fast(192000, true, DelaySettings());
   AUDIO_CHECK(slow.Prepared IS ERR::Okay and fast.Prepared IS ERR::Okay);
   AUDIO_CHECK(slow.Processor.ring_capacity() IS 16002 and slow.Processor.ring_samples() IS 16002);
   AUDIO_CHECK(fast.Processor.ring_capacity() IS 384002 and fast.Processor.ring_samples() IS 768004);

   // An unconfigured processor passes audio through and reports no tail.
   DelayFixture idle(0, true, wet_settings(10, 50));
   AUDIO_CHECK(idle.Prepared IS ERR::Okay);
   float samples[] = { 0.25f, -0.5f, 0.75f, 1.0f };
   idle.Processor.process(samples, 2);
   AUDIO_CHECK(samples[0] IS 0.25f and samples[1] IS -0.5f and samples[2] IS 0.75f and samples[3] IS 1.0f);
   AUDIO_CHECK((not idle.Processor.pending()) and (idle.Processor.tail_frames() IS 0));
   AUDIO_CHECK(idle.Processor.decay_estimate() IS 0);

   // Zero algorithmic latency and a finite tail at every setting.
   DelayFixture typical(48000, true, DelaySettings());
   AUDIO_CHECK(typical.Processor.latency() IS 0 and typical.Processor.tail() IS AudioTail::FINITE);
   AUDIO_CHECK(typical.Processor.read_distance() IS 12000.0);

   // Default estimate: the first echo, six further repeats for 60 dB at 30% feedback, then the filter's memory.
   double a, b;
   delay_damping(25, 48000, a, b);
   const uint64_t filter = uint64_t(std::ceil(std::log(1000.0) / -std::log1p(-a)));
   AUDIO_CHECK(typical.Processor.decay_estimate() IS 12000 * 7 + filter);

   // Zero feedback still includes the first echo.  The maximum settings exceed four minutes.
   AUDIO_CHECK(delay_decay_estimate(480, 0, 1, 0) IS 480);
   AUDIO_CHECK(delay_decay_estimate(96000, 0.95, 1, 0) IS uint64_t(96000) * 136);
   AUDIO_CHECK(delay_decay_estimate(96000, 0.95, 1, 0) > uint64_t(48000) * 240);
}

//********************************************************************************************************************
// Damping maps to a monotonic, rate-aware coefficient with an exact unfiltered endpoint and a 200 Hz maximum.

static void test_damping_mapping(AudioTestContext &Test)
{
   for (int rate : { 8000, 44100, 48000, 96000, 192000 }) {
      double a, b;
      delay_damping(0, rate, a, b);
      AUDIO_CHECK(a IS 1.0 and b IS 0.0);

      delay_damping(0.5, rate, a, b);
      AUDIO_CHECK(b IS 0.5 and a > 0 and a < 1);
      delay_damping(1, rate, a, b);
      AUDIO_CHECK(b IS 1.0);

      delay_damping(100, rate, a, b);
      AUDIO_CHECK(std::abs(a - (1.0 - std::exp(-2.0 * std::numbers::pi * 200.0 / double(rate)))) < 1e-15);

      const double fmax = std::min(20000.0, 0.45 * rate);
      delay_damping(50, rate, a, b);
      const double corner = -std::log(1.0 - a) * double(rate) / (2.0 * std::numbers::pi);
      AUDIO_CHECK(std::abs(corner - std::sqrt(fmax * 200.0)) < 1e-6 * corner);

      bool monotonic = true;
      double previous = 1.0;
      for (int p = 1; p <= 100; p++) {
         delay_damping(p, rate, a, b);
         monotonic &= (a < previous) and (a > 0);
         previous = a;
      }
      AUDIO_CHECK(monotonic);
   }

   // The second echo of a tone burst passes through the feedback filter once.  Damping never boosts it and removes
   // more at higher frequencies; the unfiltered first echo is unaffected.
   auto echo_level = [](double Frequency, double Damping, int Start) {
      DelayFixture fixture(48000, false, wet_settings(20, 95, Damping));
      std::vector<float> buffer(4800, 0.0f);
      for (int i = 0; i < 480; i++) buffer[i] = float(0.5 * std::sin(2.0 * std::numbers::pi * Frequency * i / 48000.0));
      fixture.run(buffer);
      double peak = 0;
      for (int i = Start + 160; i < Start + 440; i++) peak = std::max(peak, double(std::abs(buffer[i])));
      return peak;
   };
   const double bright = echo_level(10000, 100, 1920), low = echo_level(1000, 100, 1920);
   AUDIO_CHECK(bright < low and bright < echo_level(10000, 0, 1920) * 0.1);
   AUDIO_CHECK(low <= echo_level(1000, 0, 1920) * (1.0 + 1e-6));
   AUDIO_CHECK(std::abs(echo_level(10000, 100, 960) - echo_level(10000, 0, 960)) < 1e-7);
}

//********************************************************************************************************************
// Echo timing and levels against analytic values and the independent model.

static void test_timing(AudioTestContext &Test)
{
   // An integer delay reproduces the impulse exactly at each repeat, with nothing in between.
   {
      DelayFixture fixture(48000, false, wet_settings(10, 50));
      const auto output = fixture.impulse(1900);
      bool silent = true;
      for (int i = 0; i < 1900; i++) {
         if ((i IS 480) or (i IS 960) or (i IS 1440)) continue;
         silent &= output[i] IS 0;
      }
      AUDIO_CHECK(output[480] IS 1.0f and output[960] IS 0.5f and output[1440] IS 0.25f);
      AUDIO_CHECK(silent);
   }

   // A fractional delay spreads each echo across two frames with linear weights.
   {
      DelayFixture fixture(8000, false, wet_settings(60.03125, 0)); // 480.25 frames
      AUDIO_CHECK(fixture.Processor.read_distance() IS 480.25);
      const auto output = fixture.impulse(1000);
      AUDIO_CHECK(output[479] IS 0 and output[480] IS 0.75f and output[481] IS 0.25f and output[482] IS 0);
      double total = 0;
      for (auto sample : output) total += sample;
      AUDIO_CHECK(total IS 1.0);
   }

   // Zero feedback produces exactly one delayed copy at the default mix, with the dry signal unchanged.
   {
      DelaySettings settings;
      settings.Feedback = 0;
      settings.Damping = 100;
      DelayFixture fixture(48000, false, settings);
      const auto output = fixture.impulse(48000);
      AUDIO_CHECK(output[0] IS 0.75f and output[12000] IS 0.25f);
      bool single = true;
      for (int i = 1; i < 48000; i++) if (i != 12000) single &= output[i] IS 0;
      AUDIO_CHECK(single);
   }

   // The first and last frames of a maximum delay at the rate endpoints.
   for (int rate : { 8000, 192000 }) {
      DelayFixture fixture(rate, false, wet_settings(2000, 0));
      const auto output = fixture.impulse(rate * 2 + 2);
      AUDIO_CHECK(output[rate * 2] IS 1.0f and output[rate * 2 - 1] IS 0 and output[rate * 2 + 1] IS 0);
      DelayFixture shortest(rate, false, wet_settings(1, 0));
      const auto first = shortest.impulse(rate / 100);
      AUDIO_CHECK(first[rate / 1000] IS 1.0f);
   }

   // Noise through fractional delays, feedback, damping and both modes matches the independent model.
   const DelaySettings cases[] = {
      { .Time = 3.3, .Feedback = 80, .Damping = 40, .Mode = DELAY_NORMAL, .Mix = 25 },
      { .Time = 1, .Feedback = 95, .Damping = 100, .Mode = DELAY_NORMAL, .Mix = 100 },
      { .Time = 7.77, .Feedback = 60, .Damping = 0.4, .Mode = DELAY_PING_PONG, .Mix = 50 },
      { .Time = 12.5, .Feedback = 0, .Damping = 0, .Mode = DELAY_PING_PONG, .Mix = 0 }
   };
   for (int rate : { 44100, 48000, 96000 }) {
      for (int channels = 1; channels <= 2; channels++) {
         for (const auto &settings : cases) {
            DelayFixture fixture(rate, channels IS 2, settings);
            uint32_t seed = 11;
            std::vector<float> buffer(size_t(rate / 5) * channels);
            for (int i = 0; i < int(buffer.size()) / 2; i++) buffer[i] = float(noise(seed) * 0.5);
            const auto expected = reference(buffer, channels, settings, rate);
            fixture.run(buffer, 256);
            AUDIO_CHECK(max_error(buffer, expected) < 2e-5);
         }
      }
   }
}

//********************************************************************************************************************
// Zero mix is bit-identical to the input, all-wet removes the dry signal, and normal stereo keeps channels apart.

static void test_mix_and_isolation(AudioTestContext &Test)
{
   for (int channels = 1; channels <= 2; channels++) {
      auto settings = wet_settings(5, 90, 50);
      settings.Mix = 0;
      DelayFixture fixture(48000, channels IS 2, settings);
      uint32_t seed = 7;
      std::vector<float> input(size_t(4801) * channels), output;
      for (auto &sample : input) sample = float(noise(seed) * 4.0); // Headroom above unity is preserved
      output = input;
      fixture.run(output, 17);
      bool identical = true;
      for (size_t i = 0; i < input.size(); i++) identical &= output[i] IS input[i];
      AUDIO_CHECK(identical);
      AUDIO_CHECK(fixture.Processor.pending()); // History continues to update
   }

   DelayFixture wet(48000, false, wet_settings(10, 0));
   float frame = 0.5f;
   wet.Processor.process(&frame, 1);
   AUDIO_CHECK(frame IS 0);

   // Left-only input never reaches the right channel in normal mode.
   DelayFixture stereo(48000, true, wet_settings(4, 80, 30));
   const auto output = stereo.impulse(9600, 1.0f, 0.0f);
   bool isolated = true;
   double left = 0;
   for (int i = 0; i < 9600; i++) {
      isolated &= output[i * 2 + 1] IS 0;
      left = std::max(left, double(std::abs(output[i * 2])));
   }
   AUDIO_CHECK(isolated and left > 0.5);
}

//********************************************************************************************************************
// Ping-pong injects the stereo average on the left and alternates sides; mono uses normal routing.

static void test_ping_pong(AudioTestContext &Test)
{
   auto settings = wet_settings(10, 50);
   settings.Mode = DELAY_PING_PONG;
   DelayFixture fixture(48000, true, settings);
   auto output = fixture.impulse(1900, 1.0f, 0.5f);
   auto at = [&](int Frame, int Channel) { return output[size_t(Frame) * 2 + Channel]; };
   AUDIO_CHECK(at(480, 0) IS 0.75f and at(480, 1) IS 0);
   AUDIO_CHECK(at(960, 0) IS 0 and at(960, 1) IS 0.375f);
   AUDIO_CHECK(at(1440, 0) IS 0.1875f and at(1440, 1) IS 0);
   double energy = 0;
   for (int i = 0; i < 1900; i++) {
      if ((i IS 480) or (i IS 960) or (i IS 1440)) continue;
      energy += double(at(i, 0)) * at(i, 0) + double(at(i, 1)) * at(i, 1);
   }
   AUDIO_CHECK(energy IS 0);

   // The dry signal is unchanged at partial mix.
   settings.Mix = 30;
   DelayFixture partial(48000, true, settings);
   output = partial.impulse(10, 0.8f, -0.2f);
   AUDIO_CHECK(std::abs(at(0, 0) - 0.56f) < 1e-7 and std::abs(at(0, 1) + 0.14f) < 1e-7);

   // Opposite polarity cancels in the injection average.
   settings.Mix = 100;
   DelayFixture opposite(48000, true, settings);
   output = opposite.impulse(2000, 1.0f, -1.0f);
   bool silent = true;
   for (auto sample : output) silent &= sample IS 0;
   AUDIO_CHECK(silent);

   // Mono retains the requested mode but uses normal routing.
   DelayFixture mono(48000, false, settings);
   AUDIO_CHECK(mono.Processor.Settings.Mode IS DELAY_PING_PONG and mono.Processor.routing_weight() IS 0);
   output = mono.impulse(1000);
   AUDIO_CHECK(output[480] IS 1.0f and output[960] IS 0.5f);
}

//********************************************************************************************************************
// Maximum feedback with sustained input approaches but never exceeds the 1 / (1 - g) headroom, including across
// mode crossfades, and decays afterwards.  Non-finite and huge input cannot corrupt the state.

static void test_stability(AudioTestContext &Test)
{
   for (double damping : { 0.0, 100.0 }) {
      DelayFixture fixture(48000, true, wet_settings(1, 95, damping));
      std::vector<float> buffer(size_t(48000) * 2, 1.0f);
      fixture.run(buffer, 256);
      double peak = 0;
      bool finite = true;
      for (auto sample : buffer) {
         peak = std::max(peak, double(std::abs(sample)));
         finite &= std::isfinite(sample);
      }
      AUDIO_CHECK(finite and peak <= 20.0 * (1.0 + 1e-5) and peak > 19.9);
   }

   // Toggling the routing under full-scale noise remains within the same headroom.
   DelayFixture toggled(48000, true, wet_settings(2.5, 95, 10));
   uint32_t seed = 5;
   double peak = 0;
   for (int block = 0; block < 40; block++) {
      auto settings = wet_settings(2.5, 95, 10);
      settings.Mode = (block & 1) ? DELAY_PING_PONG : DELAY_NORMAL;
      toggled.change(settings);
      std::vector<float> buffer(size_t(1200) * 2);
      for (auto &sample : buffer) sample = float(noise(seed));
      toggled.run(buffer, 64);
      for (auto sample : buffer) peak = std::max(peak, double(std::abs(sample)));
   }
   AUDIO_CHECK(peak <= 20.0 * (1.0 + 1e-5));

   // Non-finite samples are silence; huge finite samples are clamped and remain finite through the loop.
   DelayFixture hostile(48000, true, wet_settings(1, 95));
   std::vector<float> buffer(size_t(4800) * 2, 0.0f);
   buffer[0] = std::numeric_limits<float>::quiet_NaN();
   buffer[1] = std::numeric_limits<float>::infinity();
   hostile.run(buffer);
   bool silent = true;
   for (auto sample : buffer) silent &= sample IS 0;
   AUDIO_CHECK(silent and (not hostile.Processor.pending()));

   std::fill(buffer.begin(), buffer.end(), std::numeric_limits<float>::max());
   hostile.run(buffer);
   bool finite = true;
   for (auto sample : buffer) finite &= std::isfinite(sample) and std::abs(sample) <= 2.1e31f;
   AUDIO_CHECK(finite);
}

//********************************************************************************************************************
// Pending output covers silent gaps and the full decay, ends within the published bound, is independent of the
// input scale, and leaves no residual history.

static void test_tail(AudioTestContext &Test)
{
   // No input: immediately idle.
   DelayFixture empty(48000, true, DelaySettings());
   std::vector<float> silence(size_t(4800) * 2, 0.0f);
   empty.run(silence);
   AUDIO_CHECK(not empty.Processor.pending());

   // A two-second gap between echoes is pending throughout.
   {
      DelayFixture fixture(8000, false, wet_settings(2000, 50));
      float sample = 1.0f;
      fixture.Processor.process(&sample, 1);
      bool held = true;
      for (int i = 1; i < 16000; i++) {
         sample = 0;
         fixture.Processor.process(&sample, 1);
         held &= fixture.Processor.pending() and (sample IS 0);
      }
      sample = 0;
      fixture.Processor.process(&sample, 1);
      AUDIO_CHECK(held and (sample IS 1.0f));
   }

   // The bound covers the observed drain, including sustained build-up at maximum feedback and slow damping.
   struct Case { int Rate; double Time, Feedback, Damping; bool Sustain; };
   const Case cases[] = {
      { 8000, 1, 95, 100, true }, { 8000, 1, 95, 0, true }, { 48000, 10, 50, 25, false },
      { 48000, 3, 90, 100, true }, { 48000, 5, 0, 100, true }, { 48000, 5, 0, 0, false },
      { 44100, 2.2, 70, 0.5, true }
   };
   for (const auto &c : cases) {
      DelayFixture fixture(c.Rate, true, wet_settings(c.Time, c.Feedback, c.Damping));
      std::vector<float> input(size_t(c.Sustain ? c.Rate : 1) * 2, 1.0f);
      fixture.run(input, 256);
      const auto bound = fixture.Processor.tail_frames();
      const auto drained = fixture.drain(bound + 1);
      AUDIO_CHECK((not fixture.Processor.pending()) and (drained <= bound));
      // The residual threshold is -120 dB, followed by a full memory span of quiet.
      const double repeats = (c.Feedback > 0) ? std::log(1e-6) / std::log(c.Feedback / 100.0) : 0;
      // Damping spreads each echo, so only undamped repeats have an exact lower bound.
      const double echoes = (c.Damping > 0) ? 0 : std::floor(repeats) * c.Time * c.Rate / 1000.0;
      AUDIO_CHECK(double(drained) >= double(delay_capacity(c.Rate)) + echoes);
   }

   // The drain duration is the same at every input scale, so quiet sources are not cut short.
   uint64_t durations[4];
   const float scales[4] = { 1.0f, 0x1p-10f, 0x1p-20f, 0x1p-40f };
   for (int i = 0; i < 4; i++) {
      DelayFixture fixture(48000, false, wet_settings(10, 60, 30));
      float sample = scales[i];
      fixture.Processor.process(&sample, 1);
      durations[i] = fixture.drain(fixture.Processor.tail_frames() + 1);
   }
   AUDIO_CHECK(durations[0] IS durations[1] and durations[0] IS durations[2] and durations[0] IS durations[3]);

   // Retired history cannot be revealed by a later edit.
   DelayFixture fixture(48000, false, wet_settings(10, 50, 0));
   auto output = fixture.impulse(10);
   fixture.drain(fixture.Processor.tail_frames() + 1);
   auto louder = wet_settings(9, 95, 0);
   fixture.change(louder);
   std::vector<float> after(4800, 0.0f);
   fixture.run(after);
   bool silent = true;
   for (auto sample : after) silent &= sample IS 0;
   AUDIO_CHECK(silent and (not fixture.Processor.pending()));
}

//********************************************************************************************************************
// Reset discards echoes and transitions; history survives ordinary edits, including mix changes in a silent gap.

static void test_lifecycle(AudioTestContext &Test)
{
   DelayFixture fixture(48000, true, wet_settings(10, 50, 25));
   fixture.impulse(100);
   auto edit = wet_settings(20, 50, 25);
   fixture.change(edit);
   AUDIO_CHECK(fixture.Processor.pending() and fixture.Processor.crossfading());

   fixture.Processor.reset();
   AUDIO_CHECK((not fixture.Processor.pending()) and (not fixture.Processor.crossfading()));
   AUDIO_CHECK(fixture.Processor.read_distance() IS 960.0);
   std::vector<float> after(size_t(4800) * 2, 0.0f);
   fixture.run(after);
   bool silent = true;
   for (auto sample : after) silent &= sample IS 0;
   AUDIO_CHECK(silent);

   // A mix increase during the gap reveals the echo recorded at zero mix.
   auto hidden = wet_settings(20, 0);
   hidden.Mix = 0;
   DelayFixture gap(48000, false, hidden);
   auto output = gap.impulse(100);
   AUDIO_CHECK(output[0] IS 1.0f);
   gap.change(wet_settings(20, 0));
   output.assign(1000, 0.0f);
   gap.run(output);
   AUDIO_CHECK(output[860] IS 1.0f); // Frame 960 since the impulse, after the mix ramp has finished

   // An unsupported target on update leaves the settings committed for the next reset.
   const float *storage = gap.Processor.ring_storage();
   gap.Processor.update(wet_settings(30, 10), nullptr);
   AUDIO_CHECK(gap.Processor.Settings.Time IS 30 and gap.Processor.read_distance() IS 960.0);
   gap.Processor.reset();
   AUDIO_CHECK(gap.Processor.read_distance() IS 1440.0 and gap.Processor.ring_storage() IS storage);
}

//********************************************************************************************************************
// Time edits use fixed read heads and a single queued target; other values ramp from their current state.

static void test_edits(AudioTestContext &Test)
{
   DelayFixture fixture(48000, true, wet_settings(10, 30, 0));
   fixture.impulse(48);

   auto first = wet_settings(20, 30, 0);
   fixture.change(first);
   AUDIO_CHECK(fixture.Processor.crossfading() and (not fixture.Processor.time_queued()));
   AUDIO_CHECK(fixture.Processor.read_distance() IS 960.0);

   auto second = wet_settings(30, 30, 0);
   fixture.change(second);
   auto third = wet_settings(40, 60, 50);
   third.Mode = DELAY_PING_PONG;
   fixture.change(third);
   AUDIO_CHECK(fixture.Processor.time_queued() and fixture.Processor.read_distance() IS 960.0);

   // The bound accounts for the queued, longer time and the higher feedback before they take effect.
   DelayTarget queued;
   delay_target(third, 48000, queued);
   AUDIO_CHECK(fixture.Processor.tail_frames() >= queued.TailFrames);

   // The queued time starts on the frame after the first 10 ms crossfade ends.
   std::vector<float> buffer(size_t(480) * 2, 0.0f);
   fixture.run(buffer, 7);
   AUDIO_CHECK(fixture.Processor.time_queued() and fixture.Processor.read_distance() IS 960.0);
   AUDIO_CHECK(fixture.Processor.feedback_gain() IS 0.6 and fixture.Processor.routing_weight() IS 1.0);
   float frame[2] = { 0, 0 };
   fixture.Processor.process(frame, 1);
   AUDIO_CHECK(fixture.Processor.crossfading() and fixture.Processor.read_distance() IS 1920.0);
   fixture.run(buffer, 7);
   AUDIO_CHECK((not fixture.Processor.crossfading()) and (not fixture.Processor.time_queued()));

   // An edit back to the crossfade's destination replaces the queued target with nothing.
   auto brief = third;
   brief.Time = 5;
   fixture.change(brief);
   fixture.change(third);
   AUDIO_CHECK(fixture.Processor.time_queued());
   fixture.change(brief);
   AUDIO_CHECK(fixture.Processor.crossfading() and (not fixture.Processor.time_queued()));

   // Feedback, damping and mix ramp over 10 ms and retarget from the current value.
   DelayFixture ramp(48000, false, wet_settings(10, 0, 0));
   ramp.change(wet_settings(10, 80, 100));
   std::vector<float> half(240, 0.0f);
   ramp.run(half);
   const double midway = ramp.Processor.feedback_gain();
   AUDIO_CHECK(std::abs(midway - 0.4) < 1e-9 and ramp.Processor.filter_blend() > 0.49);
   ramp.change(wet_settings(10, 0, 0));
   ramp.run(half);
   AUDIO_CHECK(std::abs(ramp.Processor.feedback_gain() - midway * 0.5) < 1e-9);
   ramp.run(half);
   AUDIO_CHECK(ramp.Processor.feedback_gain() IS 0 and ramp.Processor.filter_coefficient() IS 1.0);
}

//********************************************************************************************************************
// Block boundaries do not change the output, including simultaneous time, mode, feedback and damping edits and a
// queued time target.

static void test_blocks(AudioTestContext &Test)
{
   const auto initial = DelaySettings { .Time = 7.3, .Feedback = 70, .Damping = 20, .Mode = DELAY_NORMAL, .Mix = 40 };
   DelaySettings edits[3] = { initial, initial, initial };
   edits[0].Time = 21.7;
   edits[0].Mode = DELAY_PING_PONG;
   edits[0].Feedback = 90;
   edits[1].Time = 3.1;           // Queued behind the first crossfade
   edits[1].Damping = 100;
   edits[2].Mode = DELAY_NORMAL;
   edits[2].Mix = 80;
   edits[2].Time = 13;

   uint32_t seed = 3;
   std::vector<float> source(size_t(19200) * 2);
   for (int i = 0; i < 9600 * 2; i++) source[i] = float(noise(seed) * 0.5);

   const int stops[] = { 4800, 4900, 5100, 19200 };
   auto render = [&](const std::vector<int> &Blocks) {
      DelayFixture fixture(48000, true, initial);
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

   const auto whole = render({ 19200 });
   bool identical = true, finite = true;
   for (const auto &blocks : { std::vector<int>{ 1 }, std::vector<int>{ 17 }, std::vector<int>{ 256 },
         std::vector<int>{ 3, 250, 1, 64, 999 } }) {
      const auto split = render(blocks);
      for (size_t i = 0; i < whole.size(); i++) identical &= whole[i] IS split[i];
   }
   for (auto sample : whole) finite &= std::isfinite(sample) and std::abs(sample) < 20;
   AUDIO_CHECK(identical and finite);
}

//********************************************************************************************************************
// Live edits introduce no step larger than the signal's own sample-to-sample movement.

static void test_transitions(AudioTestContext &Test)
{
   auto settings = wet_settings(50, 60, 10);
   settings.Mix = 50;
   DelayFixture fixture(48000, true, settings);
   const int frames = 48000;
   std::vector<float> buffer(size_t(frames) * 2);
   for (int i = 0; i < frames; i++) {
      buffer[i * 2] = float(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * i / 48000.0));
      buffer[i * 2 + 1] = float(0.5 * std::sin(2.0 * std::numbers::pi * 330.0 * i / 48000.0));
   }

   const int edit_at = 24000;
   fixture.Processor.process(buffer.data(), edit_at);
   auto edited = settings;
   edited.Time = 137;
   edited.Mode = DELAY_PING_PONG;
   edited.Feedback = 80;
   edited.Damping = 70;
   edited.Mix = 70;
   fixture.change(edited);
   fixture.Processor.process(buffer.data() + edit_at * 2, frames - edit_at);

   auto max_step = [&](int Start, int End) {
      double step = 0;
      for (int i = Start; i < End; i++) {
         for (int c = 0; c < 2; c++) {
            step = std::max(step, double(std::abs(buffer[i * 2 + c] - buffer[(i - 1) * 2 + c])));
         }
      }
      return step;
   };

   const double before = max_step(12000, edit_at);
   const double during = max_step(edit_at, edit_at + 960);
   AUDIO_CHECK(before > 0 and during < before * 2.0);
}

//********************************************************************************************************************
// Storage never changes after preparation, and the worst-case render and reset costs at 192 kHz stereo are logged.

static void test_resources(AudioTestContext &Test)
{
   DelayFixture fixture(192000, true, DelaySettings { .Time = 2000, .Feedback = 95, .Damping = 100, .Mix = 50 });
   const float *storage = fixture.Processor.ring_storage();
   const size_t samples = fixture.Processor.ring_samples();

   uint32_t seed = 17;
   std::vector<float> buffer(size_t(1024) * 2);
   double worst_block = 0;
   for (int block = 0; block < 400; block++) {
      // Overlapping transitions: a time edit every block queues behind the running crossfade.
      auto settings = DelaySettings { .Time = 1000.0 + (block % 7) * 150.0, .Feedback = 95, .Damping = 100,
         .Mode = (block & 1) ? DELAY_PING_PONG : DELAY_NORMAL, .Mix = 50 };
      fixture.change(settings);
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
   kt::Log("AudioTests").msg("Delay at 192 kHz stereo: %.1f KB ring, worst 1024-frame block %.1f us, reset %.2f us",
      double(samples * sizeof(float)) / 1024.0, worst_block, reset.count());
}

//********************************************************************************************************************

static void run(AudioTestContext &Test)
{
   test_configuration(Test);
   test_damping_mapping(Test);
   test_timing(Test);
   test_mix_and_isolation(Test);
   test_ping_pong(Test);
   test_stability(Test);
   test_tail(Test);
   test_lifecycle(Test);
   test_edits(Test);
   test_blocks(Test);
   test_transitions(Test);
   test_resources(Test);
}

} // namespace audio_tests_audio_delay_dsp
