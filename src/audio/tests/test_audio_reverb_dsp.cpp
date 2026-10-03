// Included by audio.cpp to exercise the module implementation.

namespace audio_tests_audio_reverb_dsp {

// Configure exactly as activation does: prepare outside the lock, publish, then reset on the render thread.

struct ReverbFixture {
   extAudioEffect Effect{nullptr, 1};
   ReverbProcessor Processor;
   ERR Prepared;

   ReverbFixture(int Rate, bool Stereo, const ReverbSettings &Settings) : Processor(&Effect, Settings) {
      Effect.OutputRate = Rate;
      Effect.Stereo = Stereo;
      Effect.ResetPending = false;
      std::unique_ptr<AudioEffectConfiguration> config;
      Prepared = Processor.prepare(Rate, Stereo, config);
      if (config) config->publish();
      Processor.reset();
   }

   // Parameter updates derive their target on the control thread, as ReverbUpdate does.
   void change(const ReverbSettings &Settings) {
      ReverbTarget target;
      const bool valid = reverb_target(Settings, Processor.Rate, target);
      Processor.update(Settings, valid ? &target : nullptr);
   }

   std::vector<float> impulse(int Frames) {
      const int channels = Effect.Stereo ? 2 : 1;
      std::vector<float> buffer(size_t(Frames) * channels, 0.0f);
      buffer[0] = 1.0f;
      Processor.process(buffer.data(), Frames);
      return buffer;
   }
};

static ReverbSettings wet_settings()
{
   ReverbSettings settings;
   settings.Mix = 100;
   settings.PreDelay = 0;
   settings.Damping = 0;
   return settings;
}

static double window_energy(const std::vector<float> &Buffer, int Channels, int Start, int Frames)
{
   double energy = 0;
   for (int i = Start * Channels; i < (Start + Frames) * Channels; i++) energy += double(Buffer[i]) * Buffer[i];
   return energy;
}

static double noise(uint32_t &Seed)
{
   Seed = Seed * 1664525u + 1013904223u;
   return double(Seed >> 8) / double(1u << 23) - 1.0;
}

//********************************************************************************************************************

static void test_configuration(AudioTestContext &Test)
{
   ReverbFixture low(4000, false, ReverbSettings());
   AUDIO_CHECK(low.Prepared IS ERR::NoSupport);
   ReverbFixture high(384000, true, ReverbSettings());
   AUDIO_CHECK(high.Prepared IS ERR::NoSupport);

   // An unconfigured processor passes audio through and reports no tail.
   ReverbFixture idle(0, true, wet_settings());
   AUDIO_CHECK(idle.Prepared IS ERR::Okay);
   float samples[] = { 0.25f, -0.5f, 0.75f, 1.0f };
   idle.Processor.process(samples, 2);
   AUDIO_CHECK(samples[0] IS 0.25f and samples[1] IS -0.5f and samples[2] IS 0.75f and samples[3] IS 1.0f);
   AUDIO_CHECK((not idle.Processor.pending()) and (idle.Processor.tail_frames() IS 0));
   AUDIO_CHECK(idle.Processor.decay_estimate() IS 0);

   // The estimate covers the pre-delay, the longest line and the 60 dB decay time.
   ReverbFixture typical(48000, true, ReverbSettings());
   const auto &target = typical.Processor.Target;
   AUDIO_CHECK(typical.Processor.decay_estimate() IS uint64_t(960 + target.Lengths[REVERB_LINES - 1] + 72000));

   // Audible early reflections add their length to the estimate and the bound.
   auto early_settings = ReverbSettings();
   early_settings.EarlyLevel = 50;
   ReverbFixture early(48000, true, early_settings);
   AUDIO_CHECK(early.Processor.decay_estimate() IS typical.Processor.decay_estimate() + 1440);
   early_settings.EarlyLength = 100;
   ReverbFixture long_early(48000, true, early_settings);
   AUDIO_CHECK(long_early.Processor.tail_frames() IS typical.Processor.tail_frames() + 4800 - 1440);

   // A queued size target includes the current transition's remainder and the full fade that follows it.
   auto first = ReverbSettings();
   first.Size = 80;
   typical.change(first);
   float silence[1200 * 2] = {};
   typical.Processor.process(silence, 1200);
   auto queued = first;
   queued.Size = 20;
   queued.Decay = 100;
   typical.change(queued);
   ReverbTarget first_target;
   AUDIO_CHECK(reverb_target(first, 48000, first_target));
   AUDIO_CHECK(typical.Processor.decay_estimate() IS
      uint64_t(3600 + 4800 + 960 + first_target.Lengths[REVERB_LINES - 1] + 4800));

   // Lengths are distinct primes, scale with rate and size, and never exceed the size-100 capacity.
   std::array<int, REVERB_LINES> small, large, capacity, fast;
   reverb_lengths(0, 48000, small);
   reverb_lengths(100, 48000, capacity);
   reverb_lengths(100, 96000, fast);
   reverb_lengths(50, 48000, large);
   for (int i = 0; i < REVERB_LINES; i++) {
      AUDIO_CHECK(reverb_prime(small[i]) IS small[i]);
      AUDIO_CHECK(small[i] < large[i] and large[i] <= capacity[i]);
      AUDIO_CHECK(std::abs(double(fast[i]) / double(capacity[i]) - 2.0) < 0.02); // Prime rounding moves up to ~1%
      if (i) AUDIO_CHECK(small[i] > small[i - 1]);
   }

   // A damping pole reproduces the requested magnitude ratio at the reference frequency.
   const double omega = 2.0 * std::numbers::pi * 4000.0 / 48000.0;
   const double pole = reverb_pole(0.25, omega);
   const double magnitude = (1.0 - pole) / std::abs(std::complex<double>(1.0, 0.0) - pole * std::polar(1.0, -omega));
   AUDIO_CHECK(pole > 0 and pole < 1 and std::abs(magnitude - 0.25) < 1e-9);
   AUDIO_CHECK(reverb_pole(1.0, omega) IS 0);
}

//********************************************************************************************************************
// Zero mix is bit-identical to the input, including for mono, and never writes beyond the buffer.

static void test_passthrough(AudioTestContext &Test)
{
   for (int channels = 1; channels <= 2; channels++) {
      ReverbSettings settings;
      settings.Mix = 0;
      ReverbFixture fixture(48000, channels IS 2, settings);
      uint32_t seed = 7;
      std::vector<float> input(4801 * channels + 1), output;
      for (auto &sample : input) sample = float(noise(seed));
      input.back() = 12345.0f;
      output = input;
      fixture.Processor.process(output.data(), 4801);
      bool identical = true;
      for (size_t i = 0; i < input.size(); i++) identical &= output[i] IS input[i];
      AUDIO_CHECK(identical);
      AUDIO_CHECK(fixture.Processor.pending());
   }
}

//********************************************************************************************************************
// The first wet sample arrives after the pre-delay and the shortest line.  With zero diffusion the all-pass stages
// are pure delays and add their lengths.

static void test_onset(AudioTestContext &Test)
{
   for (double diffusion : { 70.0, 0.0 }) {
      auto settings = wet_settings();
      settings.PreDelay = 50;
      settings.Diffusion = diffusion;
      ReverbFixture fixture(48000, false, settings);
      const auto output = fixture.impulse(12000);
      int first = -1;
      for (int i = 0; i < 12000 and first < 0; i++) if (output[i] != 0) first = i;

      int expected = 2400 + fixture.Processor.Target.Lengths[0];
      if (diffusion IS 0) {
         for (int stage = 0; stage < REVERB_DIFFUSERS; stage++) expected += reverb_diffuser_length(0, stage, 48000);
      }
      AUDIO_CHECK(first IS expected);
   }
}

//********************************************************************************************************************
// Without damping, energy falls by 60 dB per decay period at every supported rate.

static void test_decay(AudioTestContext &Test)
{
   for (int rate : { 44100, 48000, 96000 }) {
      auto settings = wet_settings();
      settings.Decay = 1000;
      ReverbFixture fixture(rate, false, settings);
      const auto output = fixture.impulse(rate);
      const int window = rate / 10;
      const double early = window_energy(output, 1, rate * 3 / 10, window);
      const double late = window_energy(output, 1, rate * 8 / 10, window);
      const double drop = 10.0 * std::log10(early / late);
      AUDIO_CHECK(std::abs(drop - 30.0) < 2.0);
   }
}

//********************************************************************************************************************
// Damping removes high frequencies from the late tail but not from the input.

static void test_damping(AudioTestContext &Test)
{
   auto high_ratio = [](double Damping) {
      auto settings = wet_settings();
      settings.Damping = Damping;
      ReverbFixture fixture(48000, false, settings);
      const auto output = fixture.impulse(38400);
      double total = 0, difference = 0;
      for (int i = 24000; i < 33600; i++) {
         total += double(output[i]) * output[i];
         const double delta = double(output[i]) - output[i - 1];
         difference += delta * delta;
      }
      return difference / total;
   };

   const double bright = high_ratio(0), dark = high_ratio(100);
   AUDIO_CHECK(dark < bright * 0.25);
}

//********************************************************************************************************************
// Wet energy of an undamped impulse response stays near the input energy across decay and size.

static void test_energy(AudioTestContext &Test)
{
   const std::pair<double, double> cases[] = { { 100, 100 }, { 300, 50 }, { 1500, 50 }, { 5000, 0 } };
   for (int channels = 1; channels <= 2; channels++) {
      for (auto [decay, size] : cases) {
         auto settings = wet_settings();
         settings.Decay = decay;
         settings.Size = size;
         ReverbFixture fixture(48000, channels IS 2, settings);
         const int frames = int(decay * 48 * 2.5) + 4800;
         const auto output = fixture.impulse(frames);
         const double gain = 10.0 * std::log10(window_energy(output, channels, 0, frames) / double(channels));
         AUDIO_CHECK(gain > -4.0 and gain < 3.0);
      }
   }
}

//********************************************************************************************************************
// Extreme settings remain bounded for sustained full-scale noise, and stereo channels receive distinct reverberation.

static void test_stability(AudioTestContext &Test)
{
   ReverbSettings settings { .Decay = 20000, .Size = 0, .PreDelay = 250, .Damping = 0, .Diffusion = 100, .Mix = 100 };
   ReverbFixture fixture(48000, true, settings);
   uint32_t seed = 99;
   std::vector<float> buffer(96000 * 2);
   for (auto &sample : buffer) sample = float(noise(seed));
   fixture.Processor.process(buffer.data(), 96000);
   bool bounded = true;
   double correlation = 0, left = 0, right = 0;
   for (int i = 0; i < 96000; i++) {
      bounded &= std::isfinite(buffer[i * 2]) and std::abs(buffer[i * 2]) < 20;
      bounded &= std::isfinite(buffer[i * 2 + 1]) and std::abs(buffer[i * 2 + 1]) < 20;
      if (i >= 48000) {
         correlation += double(buffer[i * 2]) * buffer[i * 2 + 1];
         left += double(buffer[i * 2]) * buffer[i * 2];
         right += double(buffer[i * 2 + 1]) * buffer[i * 2 + 1];
      }
   }
   AUDIO_CHECK(bounded);
   AUDIO_CHECK(std::abs(correlation / std::sqrt(left * right)) < 0.5);

   std::fill(buffer.begin(), buffer.end(), 0.0f);
   fixture.Processor.process(buffer.data(), 96000);
   bounded = true;
   for (auto sample : buffer) bounded &= std::isfinite(sample) and std::abs(sample) < 20;
   AUDIO_CHECK(bounded);
}

//********************************************************************************************************************
// Pending output covers the audible tail, ends within the published bound, and reset discards all state.

static void test_tail(AudioTestContext &Test)
{
   auto settings = wet_settings();
   settings.Decay = 300;
   settings.PreDelay = 100;
   ReverbFixture fixture(48000, false, settings);
   AUDIO_CHECK(not fixture.Processor.pending());

   float sample = 1.0f;
   fixture.Processor.process(&sample, 1);
   const auto bound = fixture.Processor.tail_frames();
   uint64_t drained = 1;
   double last_audible = 0;
   while (fixture.Processor.pending() and drained <= bound) {
      float value = 0;
      fixture.Processor.process(&value, 1);
      if (std::abs(value) > 1e-6) last_audible = double(drained);
      drained++;
   }

   // Twice the decay reaches -120 dB; pre-delay precedes it.
   AUDIO_CHECK(not fixture.Processor.pending());
   AUDIO_CHECK(drained <= bound);
   AUDIO_CHECK(double(drained) > 4800 + 2 * 14400 * 0.8);
   AUDIO_CHECK(last_audible < double(drained));

   std::vector<float> after(4800, 0.0f);
   fixture.Processor.process(after.data(), 4800);
   double residual = 0;
   for (auto value : after) residual = std::max(residual, double(std::abs(value)));
   AUDIO_CHECK(residual < 1e-5);

   // Reset discards the residual exactly.
   fixture.Processor.reset();
   std::fill(after.begin(), after.end(), 0.0f);
   fixture.Processor.process(after.data(), 4800);
   bool silent = true;
   for (auto value : after) silent &= value IS 0;
   AUDIO_CHECK(silent and (not fixture.Processor.pending()));
}

//********************************************************************************************************************
// Block boundaries do not change the output, including parameter edits, size transitions and queued targets.

static void test_blocks(AudioTestContext &Test)
{
   auto settings = wet_settings();
   settings.Mix = 40;
   settings.PreDelay = 30;
   ReverbFixture whole(48000, true, settings), split(48000, true, settings);

   uint32_t seed = 3;
   std::vector<float> a(19200 * 2), b;
   for (auto &sample : a) sample = float(noise(seed) * 0.5);
   b = a;

   auto run = [](ReverbFixture &Fixture, float *Buffer, int Frames, int Block) {
      for (int i = 0; i < Frames; i += Block) Fixture.Processor.process(Buffer + i * 2, std::min(Block, Frames - i));
   };

   ReverbSettings edits[3] = { settings, settings, settings };
   edits[0].Decay = 4000;
   edits[0].PreDelay = 120;
   edits[1].Size = 80;         // Starts a size transition
   edits[2].Size = 20;         // Queued behind it
   edits[2].Diffusion = 30;
   edits[2].PreDelay = 5;      // Queued behind the pre-delay crossfade
   edits[0].EarlyLevel = 50;
   edits[0].EarlyLength = 60;
   edits[1].EarlyLevel = 50;
   edits[1].EarlyLength = 90;  // Queued behind the early length crossfade
   edits[2].EarlyLevel = 70;
   edits[2].EarlyLength = 10;  // Replaces the queued length

   int position = 0;
   const int stops[] = { 4800, 5000, 5200, 19200 };
   for (int stage = 0; stage < 4; stage++) {
      const int frames = stops[stage] - position;
      run(whole, a.data() + position * 2, frames, frames);
      run(split, b.data() + position * 2, frames, 37);
      position = stops[stage];
      if (stage < 3) {
         whole.change(edits[stage]);
         split.change(edits[stage]);
      }
   }

   bool identical = true, finite = true;
   for (size_t i = 0; i < a.size(); i++) {
      identical &= a[i] IS b[i];
      finite &= std::isfinite(a[i]);
   }
   AUDIO_CHECK(identical and finite);
}

//********************************************************************************************************************
// The early reflections alone, isolated as the difference between a wet output with and without them.  The network
// input does not depend on the early level, so the late reverberation cancels.

static std::vector<double> early_response(int Rate, bool Stereo, ReverbSettings Settings, const std::vector<float> &In)
{
   const int channels = Stereo ? 2 : 1;
   const int frames = int(In.size()) / channels;
   Settings.Mix = 100;
   auto with = Settings, without = Settings;
   without.EarlyLevel = 0;
   ReverbFixture a(Rate, Stereo, with), b(Rate, Stereo, without);
   auto x = In, y = In;
   a.Processor.process(x.data(), frames);
   b.Processor.process(y.data(), frames);
   std::vector<double> result(In.size());
   for (size_t i = 0; i < In.size(); i++) result[i] = double(x[i]) - double(y[i]);
   return result;
}

// Tap times follow the prime pattern after the pre-delay, never coincide within a channel, and match the measured
// impulse response at every supported rate.  Each channel's reflections carry the energy of the input.

static void test_early_taps(AudioTestContext &Test)
{
   for (int c = 0; c < 2; c++) {
      double energy = 0;
      for (int k = 0; k < REVERB_EARLY_TAPS; k++) {
         energy += glReverbEarlyGains.Gain[c][k] * glReverbEarlyGains.Gain[c][k];
         if (k) AUDIO_CHECK(glReverbEarlyGains.Gain[c][k] < glReverbEarlyGains.Gain[c][k - 1]);
      }
      AUDIO_CHECK(std::abs(energy - 1.0) < 1e-12);
   }

   // The shortest length at the lowest rate still separates every tap, and the longest fits the capacity.
   ReverbEarlyTaps taps;
   reverb_early_taps(5, REVERB_MIN_RATE, taps);
   for (int c = 0; c < 2; c++) {
      for (int k = 1; k < REVERB_EARLY_TAPS; k++) AUDIO_CHECK(taps[c][k] > taps[c][k - 1]);
   }
   reverb_early_taps(100, REVERB_MAX_RATE, taps);
   AUDIO_CHECK(taps[1][REVERB_EARLY_TAPS - 1] IS reverb_early_capacity(REVERB_MAX_RATE) - 1);

   for (int rate : { 44100, 48000, 96000 }) {
      reverb_early_taps(37, rate, taps);
      const double length = 37.0 * double(rate) / 1000.0;
      AUDIO_CHECK(taps[0][0] IS 0 and taps[1][REVERB_EARLY_TAPS - 1] IS int(std::lround(length)));
      for (int c = 0; c < 2; c++) {
         for (int k = 0; k < REVERB_EARLY_TAPS; k++) {
            const double exact = length * double(glReverbEarlyPrime[c][k] - 2) / 99.0;
            AUDIO_CHECK(std::abs(double(taps[c][k]) - exact) <= 0.5);
         }
      }

      ReverbSettings settings { .PreDelay = 40, .EarlyLevel = 100, .EarlyLength = 37 };
      const int predelay = int(std::lround(40.0 * double(rate) / 1000.0));
      std::vector<float> impulse(size_t(rate / 5) * 2, 0.0f);
      impulse[0] = impulse[1] = 1.0f;
      const auto response = early_response(rate, true, settings, impulse);

      // Every sample is either a tap at its expected gain or silent.
      bool matched = true;
      for (int c = 0; c < 2; c++) {
         std::vector<double> expected(rate / 5, 0.0);
         for (int k = 0; k < REVERB_EARLY_TAPS; k++) expected[predelay + taps[c][k]] = glReverbEarlyGains.Gain[c][k];
         for (int i = 0; i < rate / 5; i++) matched &= std::abs(response[i * 2 + c] - expected[i]) < 1e-6;
      }
      AUDIO_CHECK(matched);
   }
}

//********************************************************************************************************************
// Identical stereo input produces early reflections that are decorrelated between the channels, while mono uses the
// left pattern.

static void test_early_decorrelation(AudioTestContext &Test)
{
   ReverbSettings settings { .PreDelay = 0, .EarlyLevel = 100, .EarlyLength = 30 };
   uint32_t seed = 11;
   std::vector<float> stereo(48000 * 2), mono(48000);
   for (int i = 0; i < 48000; i++) stereo[i * 2] = stereo[i * 2 + 1] = mono[i] = float(noise(seed) * 0.5);

   const auto both = early_response(48000, true, settings, stereo);
   double correlation = 0, left = 0, right = 0;
   for (int i = 4800; i < 48000; i++) {
      correlation += both[i * 2] * both[i * 2 + 1];
      left += both[i * 2] * both[i * 2];
      right += both[i * 2 + 1] * both[i * 2 + 1];
   }
   AUDIO_CHECK(std::abs(correlation / std::sqrt(left * right)) < 0.2);

   const auto single = early_response(48000, false, settings, mono);
   double difference = 0;
   for (int i = 0; i < 48000; i++) difference = std::max(difference, std::abs(single[i] - both[i * 2]));
   AUDIO_CHECK(difference < 1e-6);
}

//********************************************************************************************************************
// At zero early level the output does not depend on the early length or its edits, and fading the early reflections
// out returns exactly to the output of a reverberator that never had them.

static void test_early_silent(AudioTestContext &Test)
{
   for (int channels = 1; channels <= 2; channels++) {
      auto settings = wet_settings();
      settings.Mix = 60;
      auto lengthy = settings, faded = settings;
      lengthy.EarlyLength = 100;
      faded.EarlyLevel = 80;
      ReverbFixture plain(48000, channels IS 2, settings), other(48000, channels IS 2, lengthy);
      ReverbFixture fading(48000, channels IS 2, faded);

      uint32_t seed = 5;
      std::vector<float> a(14400 * channels), b, f;
      for (auto &sample : a) sample = float(noise(seed) * 0.5);
      b = f = a;

      plain.Processor.process(a.data(), 4800);
      other.Processor.process(b.data(), 4800);
      fading.Processor.process(f.data(), 4800);
      lengthy.EarlyLength = 7;
      faded.EarlyLevel = 0;
      other.change(lengthy);
      fading.change(faded);
      plain.Processor.process(a.data() + 4800 * channels, 9600);
      other.Processor.process(b.data() + 4800 * channels, 9600);
      fading.Processor.process(f.data() + 4800 * channels, 9600);

      bool identical = true, restored = true, audible = false;
      for (size_t i = 0; i < a.size(); i++) identical &= a[i] IS b[i];
      for (size_t i = 0; i < size_t(4800 * channels); i++) audible |= a[i] != f[i];
      for (size_t i = size_t(5280 * channels); i < a.size(); i++) restored &= a[i] IS f[i];
      AUDIO_CHECK(identical and audible and restored);
   }
}

//********************************************************************************************************************
// An early-length edit at the crossfade boundary must supersede an older queued length.  Compare against the same
// render without the superseded edit, both for a new length and for an edit retaining the current length.

static void test_early_crossfade_boundary(AudioTestContext &Test)
{
   for (int rate : { 8000, 48000, 192000 }) {
      const int fade = rate / 100;
      for (int channels = 1; channels <= 2; channels++) {
         for (double latest : { 5.0, 50.0 }) {
            auto settings = wet_settings();
            settings.EarlyLevel = 100;
            ReverbFixture fixture(rate, channels IS 2, settings), reference(rate, channels IS 2, settings);
            settings.EarlyLength = 50;
            fixture.change(settings);
            reference.change(settings);
            settings.EarlyLength = 100;
            fixture.change(settings);
            uint32_t seed = 29;
            std::vector<float> a(fade * 4 * channels);
            for (auto &sample : a) sample = float(noise(seed) * 0.5);
            auto b = a;
            fixture.Processor.process(a.data(), fade);
            reference.Processor.process(b.data(), fade);

            settings.EarlyLength = latest;
            fixture.change(settings);
            reference.change(settings);
            fixture.Processor.process(a.data() + fade * channels, fade * 3);
            reference.Processor.process(b.data() + fade * channels, fade * 3);
            AUDIO_CHECK(a IS b);
            AUDIO_CHECK(fixture.Processor.longest_early() IS int(std::lround(latest * double(rate) / 1000.0)));
         }
      }
   }
}

//********************************************************************************************************************
// Live edits introduce no step larger than the signal's own sample-to-sample movement.

static void test_transitions(AudioTestContext &Test)
{
   auto settings = wet_settings();
   settings.EarlyLevel = 50;
   ReverbFixture fixture(48000, true, settings);
   const int frames = 48000;
   std::vector<float> buffer(frames * 2);
   for (int i = 0; i < frames; i++) {
      buffer[i * 2] = float(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * i / 48000.0));
      buffer[i * 2 + 1] = float(0.5 * std::sin(2.0 * std::numbers::pi * 330.0 * i / 48000.0));
   }

   const int edit_at = 24000;
   fixture.Processor.process(buffer.data(), edit_at);
   auto edited = settings;
   edited.Size = 95;
   edited.PreDelay = 80;
   edited.Decay = 800;
   edited.Diffusion = 20;
   edited.Mix = 60;
   edited.EarlyLevel = 90;
   edited.EarlyLength = 70;
   fixture.change(edited);
   AUDIO_CHECK(fixture.Processor.pending());
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
   const double during = max_step(edit_at, edit_at + 9600);
   AUDIO_CHECK(before > 0 and during < before * 2.0);
}

//********************************************************************************************************************

static void run(AudioTestContext &Test)
{
   test_configuration(Test);
   test_passthrough(Test);
   test_onset(Test);
   test_early_taps(Test);
   test_early_decorrelation(Test);
   test_early_silent(Test);
   test_early_crossfade_boundary(Test);
   test_decay(Test);
   test_damping(Test);
   test_energy(Test);
   test_stability(Test);
   test_tail(Test);
   test_blocks(Test);
   test_transitions(Test);
}

} // namespace audio_tests_audio_reverb_dsp
