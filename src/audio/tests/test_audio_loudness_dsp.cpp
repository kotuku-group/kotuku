// Included by audio.cpp to exercise the module implementation.

#include <chrono>
#include <limits>

namespace audio_tests_audio_loudness_dsp {

// Configure exactly as activation does: prepare outside the lock, publish, then reset on the render thread.

struct LoudnessFixture {
   extAudioEffect Effect{nullptr, 1};
   LoudnessProcessor Processor;
   ERR Prepared;

   LoudnessFixture(int Rate, bool Stereo, const LoudnessSettings &Settings) : Processor(&Effect, Settings) {
      Effect.OutputRate = Rate;
      Effect.Stereo = Stereo;
      Effect.ResetPending = false;
      std::unique_ptr<AudioEffectConfiguration> config;
      Prepared = Processor.prepare(Rate, Stereo, config);
      if (config) config->publish();
      Processor.reset();
   }

   // Parameter updates derive their target on the control thread, as LoudnessUpdate does.
   void change(const LoudnessSettings &Settings) {
      LoudnessTarget target;
      const bool valid = loudness_target(Settings, Processor.Rate, target);
      Processor.update(Settings, valid ? &target : nullptr);
   }

   int channels() const { return Effect.Stereo ? 2 : 1; }

   void process(std::vector<float> &Buffer) {
      Processor.process(Buffer.data(), int(Buffer.size()) / channels());
   }
};

// A continuous sine with independent peak levels per channel, so that consecutive buffers join without a phase step.
// A level of zero silences the channel.

struct Tone {
   int Rate;
   double Frequency;
   int64_t Frame = 0;

   std::vector<float> next(int Frames, int Channels, double Left, double Right = 0) {
      std::vector<float> buffer(size_t(Frames) * Channels);
      for (int i = 0; i < Frames; i++, Frame++) {
         const double phase = std::sin(2.0 * std::numbers::pi * Frequency * double(Frame) / double(Rate));
         buffer[size_t(i) * Channels] = float(Left * phase);
         if (Channels IS 2) buffer[size_t(i) * Channels + 1] = float(Right * phase);
      }
      return buffer;
   }
};

static double db_to_gain(double Db)
{
   return std::pow(10.0, Db / 20.0);
}

// Feed a tone through the fixture in uneven blocks.

static void feed(LoudnessFixture &Fixture, Tone &Source, double Seconds, double Left, double Right = 0)
{
   int remaining = int(std::lround(Seconds * double(Source.Rate)));
   const int blocks[] = { 997, 64, 2048, 1 };
   for (int i = 0; remaining > 0; i++) {
      const int count = std::min(remaining, blocks[i % 4]);
      auto buffer = Source.next(count, Fixture.channels(), Left, Right);
      Fixture.process(buffer);
      remaining -= count;
   }
}

static void silence(LoudnessFixture &Fixture, double Seconds)
{
   std::vector<float> buffer(size_t(std::lround(Seconds * double(Fixture.Processor.Rate))) * Fixture.channels(), 0);
   Fixture.process(buffer);
}

// Settings that measure without changing the signal.

static LoudnessSettings measure_only()
{
   return LoudnessSettings { .MaxBoost = 0, .MaxCut = 0 };
}

//********************************************************************************************************************
// At 48 kHz the K-weighting filters reproduce the coefficients published in ITU-R BS.1770.

static void test_filters(AudioTestContext &Test)
{
   const auto shelf = loudness_shelf(48000);
   AUDIO_CHECK(std::abs(shelf.B0 - 1.53512485958697) < 1e-9);
   AUDIO_CHECK(std::abs(shelf.B1 + 2.69169618940638) < 1e-9);
   AUDIO_CHECK(std::abs(shelf.B2 - 1.19839281085285) < 1e-9);
   AUDIO_CHECK(std::abs(shelf.A1 + 1.69065929318241) < 1e-9);
   AUDIO_CHECK(std::abs(shelf.A2 - 0.73248077421585) < 1e-9);

   const auto hp = loudness_highpass(48000);
   AUDIO_CHECK(hp.B0 IS 1.0 and hp.B1 IS -2.0 and hp.B2 IS 1.0);
   AUDIO_CHECK(std::abs(hp.A1 + 1.99004745483398) < 1e-9);
   AUDIO_CHECK(std::abs(hp.A2 - 0.99007225036621) < 1e-9);

   AUDIO_CHECK(loudness_lufs(0) IS LOUDNESS_FLOOR);
   AUDIO_CHECK(loudness_lufs(std::numeric_limits<double>::quiet_NaN()) IS LOUDNESS_FLOOR);
   AUDIO_CHECK(loudness_lufs(1e-300) IS LOUDNESS_FLOOR);
   AUDIO_CHECK(std::abs(loudness_lufs(1) - LOUDNESS_OFFSET) < 1e-12);
}

//********************************************************************************************************************
// Reference levels from BS.1770 and EBU Tech 3341: a 997 Hz sine at 0 dBFS in one channel measures -3.01 LUFS, and
// a 1 kHz sine at -23 dBFS in both stereo channels measures -23.0 LUFS.  With no boost or cut the output is the input.

static void test_calibration(AudioTestContext &Test)
{
   for (int rate : { 8000, 44100, 48000, 96000, 192000 }) {
      LoudnessFixture mono(rate, false, measure_only());
      AUDIO_REQUIRE(mono.Prepared IS ERR::Okay);
      Tone tone { rate, 997 };
      feed(mono, tone, 3.5, 1.0);
      AUDIO_CHECK(std::abs(mono.Processor.momentary_loudness() + 3.01) < 0.05);
      AUDIO_CHECK(std::abs(mono.Processor.short_term_loudness() + 3.01) < 0.05);

      LoudnessFixture stereo(rate, true, measure_only());
      Tone pair { rate, 1000 };
      auto input = pair.next(rate * 4, 2, db_to_gain(-23), db_to_gain(-23));
      auto output = input;
      stereo.process(output);
      AUDIO_CHECK(std::abs(stereo.Processor.momentary_loudness() + 23) < 0.05);
      AUDIO_CHECK(std::abs(stereo.Processor.short_term_loudness() + 23) < 0.05);
      AUDIO_CHECK(output IS input);
      AUDIO_CHECK(stereo.Processor.gain(0) IS 0 and stereo.Processor.gain(1) IS 0);
   }

   // Relative responses at 20 Hz, 1 kHz and 10 kHz match the magnitude response of the published 48 kHz
   // coefficients: -13.275, +0.698 and +4.042 dB.  Responses at the other rates match 48 kHz within 0.1 dB up to
   // 5 kHz.

   auto measure = [](int Rate, double Frequency) {
      LoudnessFixture fixture(Rate, false, measure_only());
      Tone tone { Rate, Frequency };
      feed(fixture, tone, 3.5, 1.0);
      return fixture.Processor.short_term_loudness();
   };

   const double low = measure(48000, 20), mid = measure(48000, 1000), high = measure(48000, 10000);
   AUDIO_CHECK(std::abs(mid - low - 13.973) < 0.02);
   AUDIO_CHECK(std::abs(high - mid - 3.344) < 0.02);
   for (int rate : { 44100, 96000, 192000 }) {
      for (double frequency : { 100.0, 1000.0, 5000.0 }) {
         AUDIO_CHECK(std::abs(measure(rate, frequency) - measure(48000, frequency)) < 0.1);
      }
   }
}

//********************************************************************************************************************
// Momentary loudness spans 400 ms and short-term loudness 3 s, both updated every 100 ms.  Until a window has filled
// after a reset, it covers the audio received so far.

static void test_windows(AudioTestContext &Test)
{
   const int rate = 48000;
   LoudnessFixture fixture(rate, false, measure_only());
   Tone tone { rate, 1000 };
   auto &p = fixture.Processor;

   // Nothing is measured until the first block completes.

   auto start = tone.next(4799, 1, db_to_gain(-20));
   fixture.process(start);
   AUDIO_CHECK(p.filled() IS 0 and p.momentary_loudness() IS LOUDNESS_FLOOR);
   start = tone.next(1, 1, db_to_gain(-20));
   fixture.process(start);
   AUDIO_CHECK(p.filled() IS 1);

   // A partly filled window reports the level of the audio received, not a level diluted by silence.

   AUDIO_CHECK(std::abs(p.short_term_loudness() + 23.01) < 0.1);
   feed(fixture, tone, 0.1, db_to_gain(-20));
   AUDIO_CHECK(std::abs(p.momentary_loudness() + 23.01) < 0.05);
   AUDIO_CHECK(std::abs(p.short_term_loudness() + 23.01) < 0.05);

   // Values change only when a block completes.

   feed(fixture, tone, 3.0, db_to_gain(-20));
   const double steady = p.short_term_loudness();
   std::vector<float> quiet(4799, 0.0f);
   fixture.process(quiet);
   AUDIO_CHECK(p.short_term_loudness() IS steady);

   // After 400 ms of silence the momentary window holds only the decay of the K-weighting filters, which a direct
   // filtering with the published 48 kHz coefficients measures at -65.70 LUFS.  The short-term window still holds
   // 2.6 s of tone.  One block later the decay has left the momentary window.

   quiet.assign(1, 0.0f);
   fixture.process(quiet);
   silence(fixture, 0.3);
   AUDIO_CHECK(std::abs(p.momentary_loudness() + 65.70) < 0.02);
   AUDIO_CHECK(std::abs(p.short_term_loudness() - (steady + 10.0 * std::log10(26.0 / 30.0))) < 0.01);
   silence(fixture, 0.1);
   AUDIO_CHECK(p.momentary_loudness() IS LOUDNESS_FLOOR);
   silence(fixture, 2.7);
   AUDIO_CHECK(p.short_term_loudness() < -80);
}

//********************************************************************************************************************
// A steady tone converges on the target, within the boost and cut limits, and the output then measures at the target.

static void test_levelling(AudioTestContext &Test)
{
   const int rate = 48000;
   struct Case { double Peak, Boost, Cut, Expected; };
   const Case cases[] = {
      { -20, 12, 12, 7.01 },   // -23.01 LUFS raised to -16
      { -30, 12, 12, 12.0 },   // -33.01 LUFS, limited by max_boost
      { -4,  12, 12, -9.0 },   // -7.01 LUFS lowered to -16
      { -4,  12, 6,  -6.0 },   // Limited by max_cut
      { -20, 0,  12, 0.0 }     // Boost disabled
   };

   for (const auto &c : cases) {
      LoudnessFixture fixture(rate, false, LoudnessSettings { .MaxBoost = c.Boost, .MaxCut = c.Cut,
         .Response = 500 });
      Tone tone { rate, 1000 };
      feed(fixture, tone, 12, db_to_gain(c.Peak));
      AUDIO_CHECK(std::abs(fixture.Processor.gain(0) - c.Expected) < 0.02);
      AUDIO_CHECK(std::abs(fixture.Processor.goal(0) - c.Expected) < 0.02);

      if ((c.Boost IS 12) and (c.Cut IS 12)) {
         LoudnessFixture meter(rate, false, measure_only());
         auto output = tone.next(rate * 4, 1, db_to_gain(c.Peak));
         fixture.process(output);
         meter.process(output);
         if (c.Expected < 12) AUDIO_CHECK(std::abs(meter.Processor.short_term_loudness() + 16) < 0.05);
      }
   }

   // Every target in the published range is reached by a tone that needs no more than the default limits.

   for (double target : { -36.0, -23.0, -14.0, -6.0 }) {
      LoudnessFixture fixture(rate, false, LoudnessSettings { .Target = target, .Response = 500 });
      Tone tone { rate, 1000 };
      const double peak = target + 3.01 - 5.0;  // 5 dB below the target
      feed(fixture, tone, 12, db_to_gain(peak));
      AUDIO_CHECK(std::abs(fixture.Processor.gain(0) - 5.0) < 0.02);
   }
}

//********************************************************************************************************************
// The applied gain follows its target with an exponential time constant of `response`.  New targets take effect
// immediately.

static void test_timing(AudioTestContext &Test)
{
   for (int rate : { 44100, 48000, 96000 }) {
      for (double response : { 500.0, 3000.0 }) {
         auto settings = measure_only();
         settings.Response = response;
         LoudnessFixture fixture(rate, false, settings);

         // 100 cycles of 1 kHz fill every 100 ms block at 48 kHz.  At other rates the block mean square varies by
         // less than 0.01 dB, which the tolerance allows for.

         Tone tone { rate, 1000 };
         feed(fixture, tone, 4, db_to_gain(-20));
         AUDIO_CHECK(fixture.Processor.gain(0) IS 0);

         settings.MaxBoost = 12;
         fixture.change(settings);
         const double goal = fixture.Processor.goal(0);
         AUDIO_CHECK(std::abs(goal - 7.01) < 0.05);
         AUDIO_CHECK(fixture.Processor.gain(0) IS 0);

         const int frames = int(std::lround(response * 0.001 * double(rate)));
         auto buffer = tone.next(frames, 1, db_to_gain(-20));
         fixture.process(buffer);
         AUDIO_CHECK(std::abs(fixture.Processor.gain(0) - goal * (1 - std::exp(-1.0))) < 0.01);

         // The gain is smooth: no frame-to-frame step exceeds the largest single smoother step.

         auto ramp = tone.next(rate / 10, 1, db_to_gain(-20));
         auto input = ramp;
         fixture.process(ramp);
         double previous = std::numeric_limits<double>::quiet_NaN();
         double largest = 0;
         for (size_t i = 0; i < ramp.size(); i++) {
            if (std::abs(input[i]) < 0.05f) continue;
            const double db = 20.0 * std::log10(double(ramp[i]) / double(input[i]));
            if (not std::isnan(previous)) largest = std::max(largest, std::abs(db - previous));
            previous = db;
         }
         AUDIO_CHECK(largest < 0.01);
      }
   }
}

//********************************************************************************************************************
// The gain is held while the measured loudness is below the gate, so silence and noise floors are not raised.

static void test_gate(AudioTestContext &Test)
{
   const int rate = 48000;
   const LoudnessSettings settings { .MaxBoost = 24, .Response = 500 };

   // A quiet noise floor below the gate is never raised.

   {
      LoudnessFixture fixture(rate, false, settings);
      uint32_t seed = 1;
      std::vector<float> noise(rate * 5);
      for (auto &sample : noise) {
         seed = seed * 1664525u + 1013904223u;
         sample = float((double(seed >> 8) / double(1u << 23) - 1.0) * db_to_gain(-90));
      }
      auto input = noise;
      fixture.process(noise);
      AUDIO_CHECK(fixture.Processor.short_term_loudness() < -70);
      AUDIO_CHECK(fixture.Processor.gain(0) IS 0 and noise IS input);
   }

   // After a sound ends, the gain is held once the momentary loudness falls below the gate.  It does not climb
   // while the short-term window empties.

   LoudnessFixture fixture(rate, false, settings);
   Tone tone { rate, 1000 };
   feed(fixture, tone, 10, db_to_gain(-26));
   const double sounding = fixture.Processor.gain(0);
   AUDIO_CHECK(std::abs(sounding - 13.01) < 0.05);

   silence(fixture, 1);
   const double held = fixture.Processor.gain(0);
   AUDIO_CHECK(held - sounding < 0.7);
   silence(fixture, 4);
   AUDIO_CHECK(fixture.Processor.gain(0) IS held);
   AUDIO_CHECK(fixture.Processor.short_term_loudness() < -70);

   // A held gain still respects a lower boost limit.

   auto lower = settings;
   lower.MaxBoost = 6;
   fixture.change(lower);
   AUDIO_CHECK(fixture.Processor.goal(0) IS 6);
   silence(fixture, 5);
   AUDIO_CHECK(std::abs(fixture.Processor.gain(0) - 6) < 1e-3);
}

//********************************************************************************************************************
// Linked operation applies one programme gain; unlinked operation levels every channel by its share of the target.

static void test_linking(AudioTestContext &Test)
{
   const int rate = 48000;
   const LoudnessSettings linked { .Response = 500 };
   const LoudnessSettings unlinked { .Response = 500, .Link = false };

   // Balanced material receives the same gain in both modes.

   for (const auto &settings : { linked, unlinked }) {
      LoudnessFixture fixture(rate, true, settings);
      Tone tone { rate, 1000 };
      feed(fixture, tone, 8, db_to_gain(-24), db_to_gain(-24));
      AUDIO_CHECK(std::abs(fixture.Processor.short_term_loudness() + 24) < 0.05);
      AUDIO_CHECK(std::abs(fixture.Processor.gain(0) - 8) < 0.02);
      AUDIO_CHECK(std::abs(fixture.Processor.goal(0) - fixture.Processor.goal(1)) < 1e-9);
   }

   LoudnessFixture fixture(rate, true, LoudnessSettings { .MaxBoost = 24, .Response = 500 });
   Tone tone { rate, 1000 };
   feed(fixture, tone, 8, db_to_gain(-20), db_to_gain(-26));
   auto &p = fixture.Processor;
   AUDIO_CHECK(p.gain(0) IS p.gain(1));

   // Programme loudness of -20 and -26 dBFS peaks: 10 * log10(10^-2.301 + 10^-2.901) = -22.04 LUFS.

   const double programme = 10.0 * std::log10(std::pow(10.0, -2.301) + std::pow(10.0, -2.901));
   AUDIO_CHECK(std::abs(p.short_term_loudness() - programme) < 0.05);
   AUDIO_CHECK(std::abs(p.gain(0) - (-16 - programme)) < 0.05);

   // Unlinking moves each channel to its own target at the response rate, without a step.

   const double before = p.gain(0);
   auto settings = LoudnessSettings { .MaxBoost = 24, .Response = 500, .Link = false };
   fixture.change(settings);
   AUDIO_CHECK(std::abs(p.goal(0) - (-16 - (-23.01 + 3.01))) < 0.05);
   AUDIO_CHECK(std::abs(p.goal(1) - (-16 - (-29.01 + 3.01))) < 0.05);
   feed(fixture, tone, 0.001, db_to_gain(-20), db_to_gain(-26));
   AUDIO_CHECK(std::abs(p.gain(0) - before) < 0.01);
   feed(fixture, tone, 6, db_to_gain(-20), db_to_gain(-26));
   AUDIO_CHECK(std::abs(p.gain(0) - 4.0) < 0.05 and std::abs(p.gain(1) - 10.0) < 0.05);

   // The programme measurements are unchanged by unlinking.

   AUDIO_CHECK(std::abs(p.short_term_loudness() - programme) < 0.05);

   // Unlinked, a silent channel is gated and keeps its gain while the other is levelled.

   LoudnessFixture single(rate, true, unlinked);
   Tone left { rate, 1000 };
   feed(single, left, 8, db_to_gain(-20), 0);
   AUDIO_CHECK(std::abs(single.Processor.gain(0) - 4.0) < 0.05);
   AUDIO_CHECK(single.Processor.gain(1) IS 0);
   AUDIO_CHECK(single.Processor.channel_short_term(1) IS LOUDNESS_FLOOR);
}

//********************************************************************************************************************

// Relinking below either loudness gate converges on a shared held gain without a step or renewed boosting.

static void test_gated_relinking(AudioTestContext &Test)
{
   const int rate = 48000;
   for (double pause : { 0.6, 4.0 }) {
      auto settings = LoudnessSettings { .MaxBoost = 24, .Response = 500, .Link = false };
      LoudnessFixture fixture(rate, true, settings);
      Tone tone { rate, 1000 };
      feed(fixture, tone, 8, db_to_gain(-20), db_to_gain(-26));
      silence(fixture, pause);
      auto &p = fixture.Processor;
      AUDIO_REQUIRE(p.momentary_loudness() < settings.Gate);
      if (pause < 3) AUDIO_CHECK(p.short_term_loudness() > settings.Gate);
      else AUDIO_CHECK(p.short_term_loudness() < settings.Gate);

      const double left = p.gain(0), right = p.gain(1);
      const double held = (left + right) / 2;
      AUDIO_REQUIRE(right - left > 5);
      settings.Link = true;
      fixture.change(settings);
      AUDIO_CHECK(p.gain(0) IS left and p.gain(1) IS right);
      AUDIO_CHECK(std::abs(p.goal(0) - held) < 1e-9 and p.goal(0) IS p.goal(1));

      silence(fixture, 1.0 / rate);
      AUDIO_CHECK(p.gain(0) > left and p.gain(1) < right);
      AUDIO_CHECK(std::abs(p.gain(0) - left) < 0.001 and std::abs(p.gain(1) - right) < 0.001);
      silence(fixture, 8);
      AUDIO_CHECK(std::abs(p.gain(0) - held) < 1e-5 and std::abs(p.gain(1) - held) < 1e-5);
      AUDIO_CHECK(std::abs(p.gain(0) - p.gain(1)) < 1e-6);

      // A live limit change still clamps the shared held target while gated.

      settings.MaxBoost = 3;
      fixture.change(settings);
      AUDIO_CHECK(p.goal(0) IS 3 and p.goal(1) IS 3);
      silence(fixture, 8);
      AUDIO_CHECK(std::abs(p.gain(0) - 3) < 1e-5 and std::abs(p.gain(1) - 3) < 1e-5);
   }
}

//********************************************************************************************************************

static void test_configuration(AudioTestContext &Test)
{
   const LoudnessSettings settings;
   for (int rate : { LOUDNESS_MIN_RATE - 1, LOUDNESS_MAX_RATE + 1, 4000 }) {
      LoudnessFixture fixture(rate, false, settings);
      AUDIO_CHECK(fixture.Prepared IS ERR::NoSupport);
   }
   for (int rate : { LOUDNESS_MIN_RATE, 11025, 22050, 32000, 44100, 48000, 88200, 96000, LOUDNESS_MAX_RATE }) {
      LoudnessFixture fixture(rate, true, settings);
      AUDIO_CHECK(fixture.Prepared IS ERR::Okay);
      LoudnessTarget target;
      AUDIO_CHECK(loudness_target(settings, rate, target) and target.BlockFrames IS int(std::lround(rate * 0.1)));
   }

   // Without a configured rate the processor passes audio through untouched.

   LoudnessFixture inactive(0, false, LoudnessSettings { .Target = -6 });
   AUDIO_CHECK(inactive.Prepared IS ERR::Okay);
   Tone tone { 48000, 1000 };
   auto buffer = tone.next(48000, 1, 0.01), original = buffer;
   inactive.process(buffer);
   AUDIO_CHECK(buffer IS original);

   // A processor is never pending, has no tail and no latency.

   LoudnessFixture fixture(48000, false, settings);
   AUDIO_CHECK(not fixture.Processor.pending() and fixture.Processor.tail() IS AudioTail::NONE);
   AUDIO_CHECK(fixture.Processor.tail_frames() IS 0 and fixture.Processor.latency() IS 0);
   AUDIO_CHECK(fixture.Processor.decay_estimate() IS 0);
}

//********************************************************************************************************************
// Non-finite and extreme samples cannot poison the measurement or the gain.

static void test_extremes(AudioTestContext &Test)
{
   const int rate = 48000;
   LoudnessFixture fixture(rate, true, LoudnessSettings { .Response = 500 });
   const float specials[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(), 1e30f, -1e30f, std::numeric_limits<float>::denorm_min() };
   std::vector<float> buffer;
   for (auto value : specials) buffer.insert(buffer.end(), { value, value });
   fixture.process(buffer);

   Tone tone { rate, 1000 };
   feed(fixture, tone, 10, db_to_gain(-20), db_to_gain(-20));
   auto &p = fixture.Processor;
   AUDIO_CHECK(std::isfinite(p.gain(0)) and std::isfinite(p.gain(1)));
   AUDIO_CHECK(std::abs(p.short_term_loudness() + 20) < 0.05);
   AUDIO_CHECK(std::abs(p.gain(0) - 4) < 0.05);

   auto output = tone.next(4800, 2, db_to_gain(-20), db_to_gain(-20));
   fixture.process(output);
   for (auto sample : output) AUDIO_CHECK(std::isfinite(sample));

   // Full-scale material at the maximum cut and boost limits stays finite.

   LoudnessFixture loud(rate, false, LoudnessSettings { .Target = -6, .MaxBoost = 24, .MaxCut = 24,
      .Response = 500 });
   Tone hot { rate, 1000 };
   feed(loud, hot, 5, 64.0);
   AUDIO_CHECK(std::abs(loud.Processor.gain(0) + 24) < 0.05);
}

//********************************************************************************************************************
// Integration with extAudioEffect: schema, meter layout, processor meter values, bypass and idle.

struct ChainFixture {
   std::shared_ptr<std::recursive_mutex> Mutex = std::make_shared<std::recursive_mutex>();
   std::shared_ptr<AudioEffectChain> Chain = std::make_shared<AudioEffectChain>(Mutex);
   extAudioEffect Effects[1] { { nullptr, 0 } };

   ChainFixture(int Rate, int Channels) {
      Chain->Rate = Rate;
      Chain->Stereo = Channels IS 2;
      const auto layout = Channels IS 2 ? std::span<const int>(glLayoutStereo) : std::span<const int>(glLayoutMono);
      Chain->Layout.assign(layout.begin(), layout.end());
   }

   ERR add(std::unique_ptr<AudioEffectProcessor> Processor, const AudioEffectSchema *Schema = nullptr) {
      auto &effect = Effects[0];
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
   AudioParamState state;
   state.Params.assign({ -16, 12, 12, 3000, -70, 1 });
   AUDIO_CHECK(validate_state(glLoudnessSchema, state, 48000) IS ERR::Okay);
   state.Params[LD_LINK] = 2;
   AUDIO_CHECK(validate_state(glLoudnessSchema, state, 48000) IS ERR::InvalidValue);
   state.Params[LD_LINK] = 0;
   state.Params[LD_RESPONSE] = 499;
   AUDIO_CHECK(validate_state(glLoudnessSchema, state, 48000) IS ERR::InvalidValue);

   ChainFixture mono(48000, 1);
   AUDIO_REQUIRE(mono.add(std::make_unique<LoudnessProcessor>(&mono.Effects[0], LoudnessSettings {}),
      &glLoudnessSchema) IS ERR::Okay);
   const char *mono_keys[] = { "input_peak_centre", "output_peak_centre", "momentary_loudness",
      "short_term_loudness", "applied_gain_centre" };
   AUDIO_REQUIRE(mono.Effects[0].Meters.size() IS 5);
   for (int i = 0; i < 5; i++) AUDIO_CHECK(std::string_view(mono.Effects[0].Meters[i].Key) IS mono_keys[i]);

   ChainFixture fixture(48000, 2);
   auto owned = std::make_unique<LoudnessProcessor>(&fixture.Effects[0], LoudnessSettings { .Response = 500 });
   auto leveller = owned.get();
   AUDIO_REQUIRE(fixture.add(std::move(owned), &glLoudnessSchema) IS ERR::Okay);
   auto &effect = fixture.Effects[0];
   const char *keys[] = { "input_peak_left", "input_peak_right", "output_peak_left", "output_peak_right",
      "momentary_loudness", "short_term_loudness", "applied_gain_left", "applied_gain_right" };
   AUDIO_REQUIRE(effect.Meters.size() IS 8);
   for (int i = 0; i < 8; i++) AUDIO_CHECK(std::string_view(effect.Meters[i].Key) IS keys[i]);
   AUDIO_CHECK(std::string_view(effect.Meters[4].Unit) IS "LUFS" and std::string_view(effect.Meters[4].Scope) IS
      "global" and std::string_view(effect.Meters[4].Semantics) IS "momentary-loudness");
   AUDIO_CHECK(std::string_view(effect.Meters[6].Unit) IS "dB" and std::string_view(effect.Meters[6].Scope) IS
      "channel" and std::string_view(effect.Meters[6].Semantics) IS "applied-gain");
   AUDIO_CHECK(effect.Meters[7].Channel IS int(SPK::FRONT_RIGHT));

   // Before the first interval, loudness meters hold the floor and gains hold unity.

   AUDIO_CHECK(effect.Meter.Values[4] IS LOUDNESS_FLOOR and effect.Meter.Values[6] IS 0);

   const auto xml = build_schema_xml(glLoudnessSchema, effect.Meters);
   AUDIO_CHECK(xml.find("<effect class=\"AudioLoudness\" version=\"1\"") != std::string::npos);
   AUDIO_CHECK(xml.find("<param key=\"link\" label=\"Link\"") != std::string::npos);
   AUDIO_CHECK(xml.find("key=\"short_term_loudness\" type=\"scalar\" label=\"Short-Term Loudness\"") !=
      std::string::npos);
   AUDIO_CHECK(xml.find("key=\"applied_gain_right\"") != std::string::npos and xml.find("slot=\"7\"") !=
      std::string::npos);

   // The first 50 ms interval completes no measurement block, so the loudness meters report the floor.

   Tone tone { 48000, 1000 };
   auto first = tone.next(2400, 2, db_to_gain(-23), db_to_gain(-23));
   int position = 0;
   for (int block : { 7, 1000, 393, 1000 }) {
      effect.process(first.data() + position * 2, block);
      position += block;
   }
   AUDIO_CHECK(effect.Meter.Sequence IS 1 and effect.Meter.Interval IS 2400);
   AUDIO_CHECK(effect.Meter.Values[4] IS LOUDNESS_FLOOR);
   AUDIO_CHECK(effect.Meter.ValueFlags[4] IS int(AMV::VALID | AMV::FLOOR));
   AUDIO_CHECK(effect.Meter.ValueFlags[6] IS int(AMV::VALID) and effect.Meter.Values[6] IS 0);

   // The second interval completes the first block.

   auto second = tone.next(2400, 2, db_to_gain(-23), db_to_gain(-23));
   effect.process(second.data(), 2400);
   AUDIO_CHECK(effect.Meter.Sequence IS 2);
   AUDIO_CHECK(std::abs(effect.Meter.Values[4] + 23) < 0.1 and effect.Meter.ValueFlags[4] IS int(AMV::VALID));
   AUDIO_CHECK(effect.Meter.Values[4] IS leveller->momentary_loudness());
   AUDIO_CHECK(effect.Meter.Values[5] IS leveller->short_term_loudness());

   // Applied gains are the gains at the end of each interval.

   for (int i = 0; i < 40; i++) {
      auto block = tone.next(2400, 2, db_to_gain(-23), db_to_gain(-23));
      effect.process(block.data(), 2400);
   }
   AUDIO_CHECK(effect.Meter.Values[6] IS leveller->gain(0) and effect.Meter.Values[7] IS leveller->gain(1));
   AUDIO_CHECK(effect.Meter.Values[6] > 3 and effect.Meter.Values[6] < 7);
   AUDIO_CHECK(effect.Meter.Values[2] > effect.Meter.Values[0]);  // The boost raises the output peak

   // Bypass leaves the buffer untouched; leaving bypass resets to unity gain.

   effect.Channel = 1 << 16;
   AUDIO_CHECK(AUDIOEFFECT_SET_Flags(&effect, AEF::BYPASS) IS ERR::Okay);
   auto bypassed = tone.next(480, 2, 0.1, 0.1), original = bypassed;
   effect.process(bypassed.data(), 480);
   AUDIO_CHECK(bypassed IS original);
   AUDIO_CHECK(AUDIOEFFECT_SET_Flags(&effect, AEF::NIL) IS ERR::Okay);
   auto resumed = tone.next(1, 2, 0.1, 0.1), resumed_input = resumed;
   effect.process(resumed.data(), 1);
   AUDIO_CHECK(leveller->gain(0) IS 0 and leveller->filled() IS 0 and resumed IS resumed_input);

   // Idle publishes a final interval and returns to unity gain.

   for (int i = 0; i < 40; i++) {
      auto block = tone.next(2400, 2, db_to_gain(-23), db_to_gain(-23));
      effect.process(block.data(), 2400);
   }
   AUDIO_CHECK(leveller->gain(0) > 0);
   std::vector<float> partial(200, 0.0f);
   effect.process(partial.data(), 100);
   effect.idle();
   AUDIO_CHECK((effect.Meter.Flags & AMF::IDLE) != AMF::NIL);
   AUDIO_CHECK(effect.Meter.Values[6] > 0);  // The final interval reports the gain before the reset
   AUDIO_CHECK(leveller->gain(0) IS 0 and leveller->short_term_loudness() IS LOUDNESS_FLOOR);
   AUDIO_CHECK(not effects_pending(*fixture.Chain));
}

//********************************************************************************************************************
// Throughput is recorded for comparison between builds; it is not a pass/fail criterion.

static void report_throughput()
{
   for (int channels = 1; channels <= 2; channels++) {
      for (bool link : { true, false }) {
         LoudnessFixture fixture(48000, channels IS 2, LoudnessSettings { .Response = 500, .Link = link });
         Tone tone { 48000, 440 };
         auto buffer = tone.next(48000, channels, 0.05, 0.02);
         const auto start = std::chrono::steady_clock::now();
         for (int i = 0; i < 10; i++) {
            auto copy = buffer;
            fixture.process(copy);
         }
         const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
         kt::Log("AudioTests").msg("Loudness %s %s: %.0fx real-time at 48 kHz.", channels IS 2 ? "stereo" : "mono",
            link ? "linked" : "unlinked", 10.0 / elapsed);
      }
   }
}

//********************************************************************************************************************

static void run(AudioTestContext &Test)
{
   test_filters(Test);
   test_calibration(Test);
   test_windows(Test);
   test_levelling(Test);
   test_timing(Test);
   test_gate(Test);
   test_linking(Test);
   test_gated_relinking(Test);
   test_configuration(Test);
   test_extremes(Test);
   test_effect_integration(Test);
   report_throughput();
}

} // namespace audio_tests_audio_loudness_dsp
