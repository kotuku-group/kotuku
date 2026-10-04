// Included by audio.cpp to exercise the module implementation.

#include <chrono>
#include <limits>

namespace audio_tests_audio_gate_dsp {

// Configure exactly as activation does: prepare outside the lock, publish, then reset on the render thread.

struct GateFixture {
   extAudioEffect Effect{nullptr, 1};
   GateProcessor Processor;
   ERR Prepared;

   GateFixture(int Rate, bool Stereo, const GateSettings &Settings) : Processor(&Effect, Settings) {
      Effect.OutputRate = Rate;
      Effect.Stereo = Stereo;
      Effect.ResetPending = false;
      std::unique_ptr<AudioEffectConfiguration> config;
      Prepared = Processor.prepare(Rate, Stereo, config);
      if (config) config->publish();
      Processor.reset();
   }

   // Parameter updates derive their target on the control thread, as GateUpdate does.
   void change(const GateSettings &Settings) {
      GateTarget target;
      const bool valid = gate_target(Settings, Processor.Rate, target);
      Processor.update(Settings, valid ? &target : nullptr);
   }

   int channels() const { return Effect.Stereo ? 2 : 1; }

   void process(std::vector<float> &Buffer) {
      Processor.process(Buffer.data(), int(Buffer.size()) / channels());
   }
};

// Open/close transitions observed by processing one frame at a time.  Frames are counted from the start of the
// fixture's input, so FirstClose is an absolute frame index.

struct GateTrace {
   int Opens = 0, Closes = 0;
   int FirstClose = -1;
   int Frame = 0;

   void process(GateFixture &Fixture, float *Buffer, int Frames) {
      const int channels = Fixture.channels();
      for (int i = 0; i < Frames; i++, Frame++) {
         const bool was_open = Fixture.Processor.is_open();
         Fixture.Processor.process(Buffer + size_t(i) * channels, 1);
         const bool now_open = Fixture.Processor.is_open();
         if (now_open and not was_open) Opens++;
         if (was_open and not now_open) {
            Closes++;
            if (FirstClose < 0) FirstClose = Frame;
         }
      }
   }

   void process(GateFixture &Fixture, std::vector<float> &Buffer) {
      process(Fixture, Buffer.data(), int(Buffer.size()) / Fixture.channels());
   }
};

// A gate with near-instant timing, no hold and no hysteresis, so that steady-state levels follow the static curve.

static GateSettings fast_settings(int Mode = GATE_GATE)
{
   return GateSettings { .Mode = Mode, .Threshold = -40, .Ratio = 2, .Range = 60, .Hysteresis = 0, .Attack = 0.1,
      .Hold = 0, .Release = 5 };
}

static double noise(uint32_t &Seed)
{
   Seed = Seed * 1664525u + 1013904223u;
   return double(Seed >> 8) / double(1u << 23) - 1.0;
}

static double db_to_gain(double Db)
{
   return std::pow(10.0, Db / 20.0);
}

// A constant-magnitude square wave keeps the detector level fixed while still exercising both polarities.

static std::vector<float> square(double Db, int Frames, int Channels)
{
   const float level = float(db_to_gain(Db));
   std::vector<float> buffer(size_t(Frames) * Channels);
   for (int i = 0; i < Frames; i++) {
      for (int c = 0; c < Channels; c++) buffer[size_t(i) * Channels + c] = (i & 1) ? -level : level;
   }
   return buffer;
}

static std::vector<float> sine(double Db, double Frequency, int Rate, int Frames)
{
   std::vector<float> buffer(Frames);
   for (int i = 0; i < Frames; i++) {
      buffer[i] = float(db_to_gain(Db) * std::sin(2.0 * std::numbers::pi * Frequency * double(i) / double(Rate)));
   }
   return buffer;
}

// The applied gain in dB for the final frame of a constant-magnitude buffer.

static double final_gain_db(const std::vector<float> &Input, const std::vector<float> &Output)
{
   return 20.0 * std::log10(double(Output.back()) / double(Input.back()));
}

static bool identical(const std::vector<float> &A, const std::vector<float> &B, size_t From = 0)
{
   if (A.size() != B.size()) return false;
   for (size_t i = From; i < A.size(); i++) if (A[i] != B[i]) return false;
   return true;
}

//********************************************************************************************************************

static void test_configuration(AudioTestContext &Test)
{
   for (int rate : { 4000, 7999, 192001, 384000 }) {
      GateFixture fixture(rate, true, GateSettings());
      AUDIO_CHECK(fixture.Prepared IS ERR::NoSupport);
   }

   // Both limits of the supported range attenuate a quiet signal and pass a loud one.
   for (int rate : { 8000, 192000 }) {
      GateFixture fixture(rate, false, fast_settings());
      AUDIO_CHECK(fixture.Prepared IS ERR::Okay);
      auto quiet = square(-50, rate / 10, 1);
      fixture.process(quiet);
      AUDIO_CHECK(std::abs(final_gain_db(square(-50, rate / 10, 1), quiet) + 60) < 1e-6);
      auto loud = square(-20, rate / 10, 1), original = loud;
      fixture.process(loud);
      AUDIO_CHECK(fixture.Processor.is_open() and loud.back() IS original.back());
   }

   // An unconfigured processor passes audio through and reports no reduction.
   GateFixture idle(0, true, fast_settings());
   AUDIO_CHECK(idle.Prepared IS ERR::Okay);
   float samples[] = { 0.25f, -0.5f, 0.0f, 0.0f };
   idle.Processor.process(samples, 2);
   AUDIO_CHECK(samples[0] IS 0.25f and samples[1] IS -0.5f and samples[2] IS 0.0f and samples[3] IS 0.0f);
   AUDIO_CHECK(idle.Processor.gain_reduction() IS 0);

   // Targets hold DSP-ready values derived for the rate.
   GateTarget target;
   AUDIO_CHECK(not gate_target(GateSettings(), 7999, target));
   AUDIO_REQUIRE(gate_target(GateSettings(), 48000, target));
   AUDIO_CHECK(target.Rate IS 48000 and target.Hold IS 2400 and target.Threshold IS -45);
   AUDIO_CHECK(target.Hysteresis IS 3 and target.Range IS 60 and target.Slope IS 1 and target.Gate IS 0);
   AUDIO_CHECK(std::abs(target.Attack - std::exp(-1.0 / 96.0)) < 1e-15);
   AUDIO_CHECK(std::abs(target.Release - std::exp(-1.0 / 7200.0)) < 1e-15);
   auto gate = GateSettings();
   gate.Mode = GATE_GATE;
   gate.Ratio = 20;
   gate.Hold = 0.01;
   AUDIO_REQUIRE(gate_target(gate, 44100, target));
   AUDIO_CHECK(target.Gate IS 1 and target.Slope IS 19 and target.Hold IS 0);

   // The detector window spans at least 25 ms at every rate.
   for (int rate : { 8000, 44100, 48000, 96000, 192000 }) {
      const int block = gate_block_frames(rate);
      AUDIO_CHECK(block * GATE_WINDOW_BLOCKS >= rate / 40 and (block - 1) * GATE_WINDOW_BLOCKS < rate / 40);
   }
   AUDIO_CHECK(gate_block_frames(48000) IS 150 and gate_block_frames(44100) IS 138);
}

//********************************************************************************************************************
// The expansion curve against the published equation, and steady-state levels of a closed expander.

static void test_expander_curve(AudioTestContext &Test)
{
   AUDIO_CHECK(gate_expansion(-30, -40, 1, 60) IS 0);
   AUDIO_CHECK(gate_expansion(-40, -40, 1, 60) IS 0);
   AUDIO_CHECK(gate_expansion(-50, -40, 1, 60) IS 10);
   AUDIO_CHECK(gate_expansion(-50, -40, 3, 60) IS 30);
   AUDIO_CHECK(gate_expansion(-50, -40, 19, 60) IS 60);   // Limited by the range
   AUDIO_CHECK(gate_expansion(-50, -40, 0, 60) IS 0);     // Ratio 1

   for (double ratio : { 1.5, 2.0, 4.0, 20.0 }) {
      for (double level : { -41.0, -45.0, -50.0, -60.0, -75.0, -130.0 }) {
         auto settings = fast_settings(GATE_EXPANDER);
         settings.Ratio = ratio;
         GateFixture fixture(48000, false, settings);
         const auto input = square(level, 24000, 1);
         auto output = input;
         fixture.process(output);
         AUDIO_CHECK(not fixture.Processor.is_open());
         const double expected = std::min(60.0, (ratio - 1) * (-40 - level));
         AUDIO_CHECK(std::abs(final_gain_db(input, output) + expected) < 1e-5);
         AUDIO_CHECK(std::abs(fixture.Processor.envelope() - expected) < 1e-5);
      }
   }

   // At or above the threshold the expander opens and becomes bit-transparent once the attack has settled.
   GateFixture open(48000, false, fast_settings(GATE_EXPANDER));
   const auto loud = square(-39, 24000, 1);
   auto loud_out = loud;
   open.process(loud_out);
   AUDIO_CHECK(open.Processor.is_open() and open.Processor.envelope() IS 0);
   AUDIO_CHECK(identical(loud, loud_out, 1000));

   // A ratio of 1 never attenuates, so the output is bit-identical from the first frame.
   auto unity = fast_settings(GATE_EXPANDER);
   unity.Ratio = 1;
   GateFixture flat(48000, true, unity);
   uint32_t seed = 4;
   std::vector<float> input(48000 * 2);
   for (int i = 0; i < 48000; i++) {
      const double envelope = (i / 4800) & 1 ? 0.5 : 0.0005;
      input[i * 2] = float(noise(seed) * envelope);
      input[i * 2 + 1] = float(noise(seed) * envelope);
   }
   auto output = input;
   flat.process(output);
   AUDIO_CHECK(identical(input, output) and flat.Processor.gain_reduction() IS 0);
}

//********************************************************************************************************************
// Gate mode applies the full range when closed, regardless of ratio.  A zero range is transparent in both modes.

static void test_gate_levels(AudioTestContext &Test)
{
   for (double range : { 10.0, 40.0, 90.0 }) {
      for (double level : { -41.0, -60.0, -150.0 }) {
         auto settings = fast_settings();
         settings.Range = range;
         std::vector<float> outputs[2];
         int index = 0;
         for (double ratio : { 1.0, 20.0 }) {
            settings.Ratio = ratio;
            GateFixture fixture(48000, false, settings);
            const auto input = square(level, 4800, 1);
            outputs[index] = input;
            fixture.process(outputs[index]);
            AUDIO_CHECK(std::abs(final_gain_db(input, outputs[index]) + range) < 1e-5);
            index++;
         }
         AUDIO_CHECK(identical(outputs[0], outputs[1]));
      }
   }

   for (int mode : { GATE_EXPANDER, GATE_GATE }) {
      auto settings = fast_settings(mode);
      settings.Range = 0;
      settings.Ratio = 20;
      GateFixture fixture(48000, true, settings);
      uint32_t seed = 9;
      std::vector<float> input(24000 * 2);
      for (int i = 0; i < 24000; i++) {
         const double envelope = (i / 2400) & 1 ? 0.3 : 0.0001;
         input[i * 2] = float(noise(seed) * envelope);
         input[i * 2 + 1] = float(noise(seed) * envelope);
      }
      auto output = input;
      fixture.process(output);
      AUDIO_CHECK(identical(input, output) and fixture.Processor.envelope() IS 0);
   }
}

//********************************************************************************************************************
// The gate opens at the threshold and closes below the threshold minus the hysteresis.  Between the two it holds its
// current state.

static void test_hysteresis(AudioTestContext &Test)
{
   struct Stage { double Level; bool OpenWith; bool OpenWithout; };
   const Stage stages[] = {
      { -30, true, true },     // Opens
      { -43, true, false },    // Between the thresholds: an open gate stays open
      { -50, false, false },   // Below the closing threshold
      { -43, false, false },   // Between the thresholds: a closed gate stays closed
      { -39, true, true }      // Reopens
   };

   for (double hysteresis : { 6.0, 0.0 }) {
      auto settings = fast_settings();
      settings.Hysteresis = hysteresis;
      GateFixture fixture(48000, false, settings);
      GateTrace trace;
      for (const auto &stage : stages) {
         const auto input = square(stage.Level, 9600, 1);
         auto output = input;
         trace.process(fixture, output);
         const bool expected = (hysteresis > 0) ? stage.OpenWith : stage.OpenWithout;
         AUDIO_CHECK(fixture.Processor.is_open() IS expected);
         if (expected) AUDIO_CHECK(identical(input, output, 1000));
         else AUDIO_CHECK(std::abs(final_gain_db(input, output) + 60) < 1e-5);
      }
      AUDIO_CHECK(trace.Opens IS 2 and trace.Closes IS 1);
   }
}

//********************************************************************************************************************
// The gate closes exactly the detector window plus the hold time after the input falls below the closing threshold.

static void test_hold(AudioTestContext &Test)
{
   for (int rate : { 44100, 48000, 96000 }) {
      const int block = gate_block_frames(rate);
      const int window = block * GATE_WINDOW_BLOCKS;
      for (int offset : { 0, block / 2 }) {
         // When the loud section ends on a sub-block boundary the window is exactly GATE_WINDOW_BLOCKS sub-blocks.
         const int loud_frames = block * 30 + offset;
         int baseline = -1;
         for (double hold : { 0.0, 10.0, 100.0 }) {
            auto settings = fast_settings();
            settings.Hold = hold;
            GateFixture fixture(rate, true, settings);
            GateTrace trace;
            auto loud = square(-20, loud_frames, 2);
            trace.process(fixture, loud);
            std::vector<float> silence(size_t(rate) * 2 / 2, 0.0f);
            trace.process(fixture, silence);
            AUDIO_CHECK(trace.Opens IS 1 and trace.Closes IS 1);

            const int hold_frames = int(std::lround(hold * 0.001 * rate));
            const int delay = trace.FirstClose - loud_frames - hold_frames;
            if (offset IS 0) AUDIO_CHECK(delay IS window);
            else AUDIO_CHECK(delay >= window and delay <= window + block);
            if (baseline < 0) baseline = trace.FirstClose;
            else AUDIO_CHECK(trace.FirstClose - baseline IS hold_frames);
         }
      }
   }
}

//********************************************************************************************************************
// Sustained tones just above the threshold never close the gate, even without hold or hysteresis, because the
// detector window spans their zero crossings.  Gaps shorter than the window plus the hold time are bridged.

static void test_chatter(AudioTestContext &Test)
{
   for (double frequency : { 25.0, 100.0, 1000.0, 9000.0 }) {
      GateFixture fixture(48000, false, fast_settings());
      GateTrace trace;
      const auto input = sine(-39.5, frequency, 48000, 48000);
      auto output = input;
      trace.process(fixture, output);
      AUDIO_CHECK(trace.Opens IS 1 and trace.Closes IS 0);
      AUDIO_CHECK(identical(input, output, 24000));
   }

   // Bursts of 30 ms above the threshold separated by 40 ms gaps.  Without hold the gate closes in every gap; a
   // 50 ms hold keeps it open throughout.
   std::vector<float> bursts(48000);
   uint32_t seed = 12;
   for (int i = 0; i < 48000; i++) bursts[i] = float(noise(seed) * ((i % 3360) < 1440 ? 0.1 : 0.001));

   for (double hold : { 0.0, 50.0 }) {
      auto settings = fast_settings();
      settings.Hold = hold;
      GateFixture fixture(48000, false, settings);
      GateTrace trace;
      auto output = bursts;
      trace.process(fixture, output);
      if (hold > 0) AUDIO_CHECK(trace.Opens IS 1 and trace.Closes IS 0);
      else AUDIO_CHECK(trace.Opens >= 14 and trace.Closes >= 13);
   }
}

//********************************************************************************************************************
// Attack and release are exponential time constants in the dB domain.

static void test_timing(AudioTestContext &Test)
{
   for (int rate : { 44100, 48000, 96000 }) {
      // Opening from the closed state that reset() establishes.
      auto settings = fast_settings();
      settings.Attack = 10;
      settings.Release = 50;
      GateFixture fixture(rate, false, settings);
      AUDIO_CHECK(fixture.Processor.envelope() IS 60 and not fixture.Processor.is_open());
      const int attack_frames = rate / 100;
      auto input = square(-20, attack_frames, 1);
      auto output = input;
      fixture.process(output);
      AUDIO_CHECK(std::abs(fixture.Processor.envelope() - 60 * std::exp(-1.0)) < 1e-9);
      AUDIO_CHECK(std::abs(final_gain_db(input, output) + fixture.Processor.envelope()) < 1e-5);

      // Closing begins one detector window after the input falls; the release time constant then applies.
      auto settle = square(-20, rate, 1);
      fixture.process(settle);
      AUDIO_CHECK(fixture.Processor.envelope() IS 0);
      const int block = gate_block_frames(rate);
      const int pad = block - ((attack_frames + rate) % block);  // Align the drop with a sub-block boundary
      auto aligned = square(-20, pad, 1);
      fixture.process(aligned);
      const int release_frames = rate / 20;
      auto quiet = square(-60, block * GATE_WINDOW_BLOCKS + release_frames, 1);
      fixture.process(quiet);
      AUDIO_CHECK(not fixture.Processor.is_open());
      AUDIO_CHECK(std::abs(fixture.Processor.envelope() - 60 * (1 - std::exp(-1.0))) < 1e-9);
   }
}

//********************************************************************************************************************
// Stereo is linked: the louder channel opens the gate and both channels receive the same gain.  Identical channels
// match the mono result.

static void test_linking(AudioTestContext &Test)
{
   const int frames = 24000;
   uint32_t seed = 31;
   std::vector<float> stereo(frames * 2), mono(frames), dual(frames * 2);
   for (int i = 0; i < frames; i++) {
      const double envelope = (i / 4000) & 1 ? 0.0003 : 0.2;
      stereo[i * 2] = float(noise(seed) * envelope);
      stereo[i * 2 + 1] = float(noise(seed) * 0.001);   // Always below the threshold
      mono[i] = float(noise(seed) * envelope);
      dual[i * 2] = dual[i * 2 + 1] = mono[i];
   }

   auto settings = fast_settings();
   settings.Hold = 5;
   GateFixture linked(48000, true, settings);
   auto output = stereo;
   linked.process(output);
   bool same_gain = true;
   for (int i = 0; i < frames; i++) {
      if ((stereo[i * 2] IS 0) or (stereo[i * 2 + 1] IS 0)) continue;
      const double left = double(output[i * 2]) / double(stereo[i * 2]);
      const double right = double(output[i * 2 + 1]) / double(stereo[i * 2 + 1]);
      same_gain &= std::abs(left - right) <= 1e-6 * std::max(left, 1e-3);
   }
   AUDIO_CHECK(same_gain);
   AUDIO_CHECK(linked.Processor.gain_reduction() > 59);
   // While the left channel is loud the right channel passes unchanged.
   bool passed = true;
   for (int i = 2000; i < 4000; i++) passed &= output[i * 2 + 1] IS stereo[i * 2 + 1];
   AUDIO_CHECK(passed);

   GateFixture mono_gate(48000, false, settings), dual_gate(48000, true, settings);
   auto mono_out = mono, dual_out = dual;
   mono_gate.process(mono_out);
   dual_gate.process(dual_out);
   bool matches = true;
   for (int i = 0; i < frames; i++) {
      matches &= (dual_out[i * 2] IS mono_out[i]) and (dual_out[i * 2 + 1] IS mono_out[i]);
   }
   AUDIO_CHECK(matches);
}

//********************************************************************************************************************
// Output, transitions and meters are independent of block size, including edits that arrive between blocks.

static void test_blocks(AudioTestContext &Test)
{
   uint32_t seed = 3;
   std::vector<float> input(28800 * 2);
   for (int i = 0; i < 28800; i++) {
      const double envelope = (i / 2400) & 1 ? 0.2 : 0.002;
      input[i * 2] = float(noise(seed) * envelope);
      input[i * 2 + 1] = float(noise(seed) * envelope);
   }

   GateSettings edits[3] = { GateSettings(), GateSettings(), GateSettings() };
   edits[0].Mode = GATE_GATE;
   edits[0].Threshold = -30;
   edits[0].Hold = 5;
   edits[1].Mode = GATE_GATE;    // During the first ramp
   edits[1].Threshold = -30;
   edits[1].Ratio = 4;
   edits[1].Hysteresis = 12;
   edits[1].Attack = 0.5;
   edits[1].Hold = 1;            // Shortens the hold in progress
   edits[2].Ratio = 4;           // Rapid replacement of the endpoint
   edits[2].Threshold = -35;
   edits[2].Hold = 2;
   edits[2].Release = 20;
   edits[2].Range = 30;

   struct Result {
      std::vector<float> Output;
      std::array<double, 4> Maximum;
      double Envelope;
      int Opens, Closes;
   };

   auto render = [&](int Pattern) {
      static const int irregular[] = { 1, 7, 64, 3, 151, 480, 2 };
      GateFixture fixture(48000, true, GateSettings());
      Result result { .Output = input, .Opens = 0, .Closes = 0 };
      int position = 0, cycle = 0;
      const int stops[] = { 4800, 5000, 5100, 28800 };
      for (int stage = 0; stage < 4; stage++) {
         result.Maximum[stage] = 0;
         while (position < stops[stage]) {
            int block = stops[stage] - position;
            if (Pattern IS 1) block = 1;
            else if (Pattern IS 2) block = std::min(block, irregular[cycle++ % std::size(irregular)]);
            const bool was_open = fixture.Processor.is_open();
            fixture.Processor.process(result.Output.data() + position * 2, block);
            if (fixture.Processor.is_open() != was_open) (was_open ? result.Closes : result.Opens)++;
            result.Maximum[stage] = std::max(result.Maximum[stage], fixture.Processor.gain_reduction());
            position += block;
         }
         if (stage < 3) fixture.change(edits[stage]);
      }
      result.Envelope = fixture.Processor.envelope();
      return result;
   };

   const auto whole = render(0);
   const auto single = render(1);
   const auto irregular = render(2);

   bool finite = true;
   for (auto sample : whole.Output) finite &= std::isfinite(sample);
   AUDIO_CHECK(finite);
   AUDIO_CHECK(identical(whole.Output, single.Output) and identical(whole.Output, irregular.Output));
   AUDIO_CHECK(whole.Envelope IS single.Envelope and whole.Envelope IS irregular.Envelope);
   AUDIO_CHECK(whole.Maximum IS single.Maximum and whole.Maximum IS irregular.Maximum);
   // Each 50 ms gap allows about one 20 ms release time constant after the window and hold: 30 * (1 - 1/e) dB.
   AUDIO_CHECK(whole.Maximum[3] > 17 and whole.Maximum[3] < 22);
   AUDIO_CHECK(single.Opens >= 4 and single.Closes >= 4);
}

//********************************************************************************************************************
// Live edits, including a mode change, alter the applied gain gradually and converge to the latest settings.

static void test_automation(AudioTestContext &Test)
{
   const int frames = 96000;
   const auto input = square(-50, frames, 2);

   // A closed gate at -50 dB applies 60 dB.  Switching to an expander with a fast attack would lift the gain to
   // -10 dB within a millisecond if the mode were not blended.
   auto start = fast_settings();
   GateFixture fixture(48000, true, start);
   auto output = input;
   auto last = fast_settings(GATE_EXPANDER);
   last.Threshold = -45;
   last.Ratio = 3;
   last.Range = 40;
   last.Hysteresis = 6;
   last.Release = 200;

   fixture.Processor.process(output.data(), 24000);
   AUDIO_CHECK(std::abs(fixture.Processor.envelope() - 60) < 1e-9);
   fixture.change(fast_settings(GATE_EXPANDER));
   fixture.Processor.process(output.data() + 24000 * 2, 200);
   fixture.change(last);
   fixture.Processor.process(output.data() + 24200 * 2, frames - 24200);

   double largest = 0;
   bool linked = true;
   for (int i = 1; i < frames; i++) {
      const double gain = double(output[i * 2]) / double(input[i * 2]);
      const double previous = double(output[(i - 1) * 2]) / double(input[(i - 1) * 2]);
      linked &= output[i * 2] IS output[i * 2 + 1];
      largest = std::max(largest, std::abs(gain - previous) / std::max(gain, previous));
   }
   AUDIO_CHECK(linked);
   AUDIO_CHECK(largest < 0.04);  // An unramped switch steps by more than 20% per frame

   // The expander target is min(40, 2 * (-45 - -50)) = 10 dB, identical to a gate that began with these settings.
   GateFixture reference(48000, true, last);
   auto expected = input;
   reference.process(expected);
   AUDIO_CHECK(std::abs(fixture.Processor.envelope() - 10) < 1e-6);
   AUDIO_CHECK(std::abs(fixture.Processor.envelope() - reference.Processor.envelope()) < 1e-9);
   AUDIO_CHECK(std::abs(output.back() - expected.back()) < 1e-9);

   // A threshold edit that opens the gate ramps rather than switching instantly.
   GateFixture threshold(48000, false, fast_settings());
   auto quiet = square(-50, 4800, 1);
   threshold.process(quiet);
   auto lowered = fast_settings();
   lowered.Threshold = -60;
   threshold.change(lowered);
   auto ramp = square(-50, 480, 1);
   GateTrace trace;
   trace.process(threshold, ramp.data(), 239);
   AUDIO_CHECK(not threshold.Processor.is_open());   // -40 to -60 over 480 frames reaches -50 half way
   trace.process(threshold, ramp.data() + 239, 241);
   AUDIO_CHECK(threshold.Processor.is_open() and trace.Opens IS 1);
}

//********************************************************************************************************************
// Reset discards ramps and history, closes the gate at the reduction for silence and adopts the latest settings.

static void test_reset(AudioTestContext &Test)
{
   auto settings = fast_settings();
   settings.Hold = 500;
   GateFixture fixture(48000, true, settings);
   auto loud = square(-10, 4800, 2);
   fixture.process(loud);
   AUDIO_CHECK(fixture.Processor.is_open() and fixture.Processor.envelope() IS 0);

   AUDIO_CHECK(not fixture.Processor.pending());
   AUDIO_CHECK(fixture.Processor.tail() IS AudioTail::NONE);
   AUDIO_CHECK(fixture.Processor.tail_frames() IS 0 and fixture.Processor.decay_estimate() IS 0);
   AUDIO_CHECK(fixture.Processor.latency() IS 0);
   std::unique_ptr<AudioEffectConfiguration> config;
   AUDIO_CHECK(fixture.Processor.prepare(48000, true, config) IS ERR::Okay and config and config->latency() IS 0);

   // An edit that is still ramping is cancelled by reset, which applies its endpoint immediately.
   auto edited = settings;
   edited.Mode = GATE_EXPANDER;
   edited.Ratio = 4;
   edited.Range = 30;
   fixture.change(edited);
   auto partial = square(-10, 100, 2);
   fixture.process(partial);
   fixture.Processor.reset();
   AUDIO_CHECK(not fixture.Processor.is_open() and fixture.Processor.gain_reduction() IS 0);
   AUDIO_CHECK(fixture.Processor.envelope() IS 30);

   GateFixture fresh(48000, true, edited);
   uint32_t seed = 8;
   std::vector<float> a(9600 * 2);
   for (int i = 0; i < 9600; i++) a[i * 2] = a[i * 2 + 1] = float(noise(seed) * (i < 4800 ? 0.0005 : 0.5));
   auto b = a;
   fixture.process(a);
   fresh.process(b);
   AUDIO_CHECK(identical(a, b));

   // With a ratio of 1 the expander has no reduction for silence.
   auto unity = edited;
   unity.Ratio = 1;
   GateFixture flat(48000, false, unity);
   AUDIO_CHECK(flat.Processor.envelope() IS 0);

   // Edits while the output is unconfigured are adopted by the next configuration.
   GateFixture unconfigured(0, false, GateSettings());
   unconfigured.change(edited);
   unconfigured.Effect.OutputRate = 48000;
   std::unique_ptr<AudioEffectConfiguration> late;
   AUDIO_REQUIRE(unconfigured.Processor.prepare(48000, false, late) IS ERR::Okay);
   late->publish();
   unconfigured.Processor.reset();
   GateFixture mono(48000, false, edited);
   auto c = square(-50, 4800, 1), d = c;
   unconfigured.process(c);
   mono.process(d);
   AUDIO_CHECK(identical(c, d));
}

//********************************************************************************************************************
// gain_reduction() reports the maximum envelope of the most recent call, reads are non-destructive, and a fully open
// call reports zero.

static void test_reduction_meter(AudioTestContext &Test)
{
   auto settings = fast_settings();
   settings.Attack = 1;
   GateFixture fixture(48000, false, settings);

   auto loud = square(-20, 4800, 1);
   fixture.process(loud);
   const double reduction = fixture.Processor.gain_reduction();
   AUDIO_CHECK(std::abs(reduction - 60 * std::exp(-1.0 / 48.0)) < 1e-9);  // The first frame
   AUDIO_CHECK(fixture.Processor.gain_reduction() IS reduction);
   AUDIO_CHECK(fixture.Processor.envelope() IS 0);

   auto more = square(-20, 480, 1);
   fixture.process(more);
   AUDIO_CHECK(fixture.Processor.gain_reduction() IS 0);

   std::vector<float> silence(4800, 0.0f);
   fixture.process(silence);
   AUDIO_CHECK(fixture.Processor.gain_reduction() > 59 and fixture.Processor.gain_reduction() < 60);
}

//********************************************************************************************************************
// Extreme settings and non-finite input stay bounded.

static void test_extremes(AudioTestContext &Test)
{
   const GateSettings extremes[] = {
      { .Mode = GATE_EXPANDER, .Threshold = -90, .Ratio = 20, .Range = 90, .Hysteresis = 24, .Attack = 0.1,
        .Hold = 0, .Release = 5 },
      { .Mode = GATE_GATE, .Threshold = 0, .Ratio = 1, .Range = 90, .Hysteresis = 0, .Attack = 100, .Hold = 1000,
        .Release = 2000 },
      { .Mode = GATE_EXPANDER, .Threshold = 0, .Ratio = 20, .Range = 90, .Hysteresis = 24, .Attack = 100,
        .Hold = 1000, .Release = 5 }
   };

   for (const auto &settings : extremes) {
      for (int rate : { 8000, 192000 }) {
         GateFixture fixture(rate, true, settings);
         uint32_t seed = 17;
         std::vector<float> input(size_t(rate) * 2);
         for (int i = 0; i < rate; i++) {
            const double envelope = (i / (rate / 8)) & 1 ? 2.0 : 1e-6;
            input[i * 2] = float(noise(seed) * envelope);
            input[i * 2 + 1] = float(noise(seed) * envelope);
         }
         auto output = input;
         fixture.process(output);
         bool bounded = true;
         for (size_t i = 0; i < output.size(); i++) {
            bounded &= std::isfinite(output[i]) and std::abs(output[i]) <= std::abs(input[i]);
         }
         AUDIO_CHECK(bounded);
         AUDIO_CHECK(fixture.Processor.envelope() >= 0 and fixture.Processor.envelope() <= 90);
      }
   }

   // A closed 90 dB gate.
   auto deep = fast_settings();
   deep.Threshold = -90;
   deep.Range = 90;
   GateFixture fixture(48000, false, deep);
   const auto input = square(-95, 4800, 1);
   auto output = input;
   fixture.process(output);
   AUDIO_CHECK(std::abs(final_gain_db(input, output) + 90) < 1e-5);

   // NaN is ignored by the detector and infinity opens the gate; neither disturbs later frames.
   for (float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() }) {
      GateFixture gate(48000, false, fast_settings());
      auto before = square(-50, 4800, 1);
      gate.process(before);
      std::vector<float> single = { bad };
      gate.process(single);
      AUDIO_CHECK(gate.Processor.is_open() IS std::isinf(bad));
      const auto after_input = square(-50, 9600, 1);
      auto after = after_input;
      gate.process(after);
      AUDIO_CHECK(std::isfinite(gate.Processor.envelope()) and not gate.Processor.is_open());
      AUDIO_CHECK(std::abs(final_gain_db(after_input, after) + 60) < 1e-5);
   }
}

//********************************************************************************************************************
// Integration with extAudioEffect: schema, meter layout, interval aggregation, bypass, idle and leaving bypass.

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
   // Defaults validate, and the ratio is declared inactive in gate mode.
   AudioParamState state;
   state.Params.assign({ 0, -45, 2, 60, 3, 2, 50, 150 });
   AUDIO_CHECK(validate_state(glGateSchema, state, 48000) IS ERR::Okay);
   state.Params[GT_MODE] = 2;
   AUDIO_CHECK(validate_state(glGateSchema, state, 48000) IS ERR::InvalidValue);

   ChainFixture mono(48000, 1);
   AUDIO_REQUIRE(mono.add(std::make_unique<GateProcessor>(&mono.Effects[0], fast_settings()), &glGateSchema) IS
      ERR::Okay);
   auto &mono_effect = mono.Effects[0];
   AUDIO_REQUIRE(mono_effect.Meters.size() IS 3);
   AUDIO_CHECK(std::string_view(mono_effect.Meters[0].Key) IS "input_peak_centre");
   AUDIO_CHECK(std::string_view(mono_effect.Meters[1].Key) IS "output_peak_centre");
   AUDIO_CHECK(std::string_view(mono_effect.Meters[2].Key) IS "gain_reduction");

   ChainFixture fixture(48000, 2);
   auto owned = std::make_unique<GateProcessor>(&fixture.Effects[0], fast_settings());
   auto gate = owned.get();
   AUDIO_REQUIRE(fixture.add(std::move(owned), &glGateSchema) IS ERR::Okay);
   auto &effect = fixture.Effects[0];
   AUDIO_REQUIRE(effect.Meters.size() IS 5);
   const char *keys[] = { "input_peak_left", "input_peak_right", "output_peak_left", "output_peak_right",
      "gain_reduction" };
   for (int i = 0; i < 5; i++) AUDIO_CHECK(std::string_view(effect.Meters[i].Key) IS keys[i]);
   AUDIO_CHECK(std::string_view(effect.Meters[4].Scope) IS "global");
   AUDIO_CHECK(std::string_view(effect.Meters[4].Semantics) IS "maximum-attenuation");

   const auto xml = build_schema_xml(glGateSchema, effect.Meters);
   AUDIO_CHECK(xml.find("<effect class=\"AudioGate\" version=\"1\"") != std::string::npos);
   AUDIO_CHECK(xml.find("<rule key=\"mode\" values=\"1\" inactive=\"1\"/>") != std::string::npos);
   AUDIO_CHECK(xml.find("key=\"gain_reduction\" type=\"scalar\"") != std::string::npos);

   // One 50 ms interval of a loud left channel, processed in uneven blocks.  The gate opens on the first frame from
   // its closed state, so the interval's reduction is the first frame's.
   auto loud = square(-20, 2400, 2);
   for (int i = 0; i < 2400; i++) loud[i * 2 + 1] *= 0.25f;
   int position = 0;
   for (int block : { 7, 1000, 393, 1000 }) {
      effect.process(loud.data() + position * 2, block);
      position += block;
   }
   AUDIO_CHECK(effect.Meter.Sequence IS 1 and effect.Meter.Interval IS 2400);
   AUDIO_CHECK(std::abs(effect.Meter.Values[0] + 20) < 1e-5);
   AUDIO_CHECK(std::abs(effect.Meter.Values[2] + 20) < 1e-5);   // The open gate is transparent
   AUDIO_CHECK(std::abs(effect.Meter.Values[4] - 60 * std::exp(-1.0 / 4.8)) < 1e-6);
   AUDIO_CHECK(effect.Meter.ValueFlags[4] IS int(AMV::VALID));

   // A silent interval closes the gate after the detector window and releases towards the range.
   std::vector<float> silence(2400 * 2, 0.0f);
   effect.process(silence.data(), 2400);
   AUDIO_CHECK(effect.Meter.Sequence IS 2 and not gate->is_open());
   AUDIO_CHECK(std::abs(effect.Meter.Values[4] - 60 * (1 - std::exp(-1200.0 / 240.0))) < 1e-6);

   // Bypass leaves the buffer untouched; leaving bypass resets the gate to its closed state.
   auto again = square(-20, 1200, 2);
   effect.process(again.data(), 1200);
   AUDIO_CHECK(gate->is_open() and gate->envelope() IS 0);
   effect.Channel = 1 << 16;
   AUDIO_CHECK(AUDIOEFFECT_SET_Flags(&effect, AEF::BYPASS) IS ERR::Okay);
   auto bypassed = square(-60, 480, 2), original = bypassed;
   effect.process(bypassed.data(), 480);
   AUDIO_CHECK(bypassed IS original);
   AUDIO_CHECK(AUDIOEFFECT_SET_Flags(&effect, AEF::NIL) IS ERR::Okay);
   std::array<float, 2> frame = { 0.001f, 0.001f };
   effect.process(frame.data(), 1);
   AUDIO_CHECK(not gate->is_open() and gate->envelope() IS 60);
   AUDIO_CHECK(std::abs(frame[0] - 1e-6f) < 1e-10f);

   // Idle discards the open state.
   again = square(-20, 1200, 2);
   effect.process(again.data(), 1200);
   AUDIO_CHECK(gate->is_open());
   effect.idle();
   AUDIO_CHECK(not gate->is_open() and gate->envelope() IS 60 and gate->gain_reduction() IS 0);
   AUDIO_CHECK((effect.Meter.Flags & AMF::IDLE) != AMF::NIL);
   AUDIO_CHECK(not effects_pending(*fixture.Chain));
}

//********************************************************************************************************************
// Throughput is recorded for comparison between builds; it is not a pass/fail criterion.

static void report_throughput()
{
   for (int channels = 1; channels <= 2; channels++) {
      GateFixture fixture(48000, channels IS 2, GateSettings());
      uint32_t seed = 1;
      std::vector<float> buffer(48000 * channels);
      for (int i = 0; i < 48000; i++) {
         const double envelope = (i / 4800) & 1 ? 0.5 : 0.002;
         for (int c = 0; c < channels; c++) buffer[i * channels + c] = float(noise(seed) * envelope);
      }
      const auto start = std::chrono::steady_clock::now();
      for (int second = 0; second < 10; second++) {
         auto copy = buffer;
         fixture.process(copy);
      }
      const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      kt::Log("AudioTests").msg("Gate %s: %.0fx real-time at 48 kHz.", channels IS 2 ? "stereo" : "mono",
         10.0 / elapsed);
   }
}

//********************************************************************************************************************

static void run(AudioTestContext &Test)
{
   test_configuration(Test);
   test_expander_curve(Test);
   test_gate_levels(Test);
   test_hysteresis(Test);
   test_hold(Test);
   test_chatter(Test);
   test_timing(Test);
   test_linking(Test);
   test_blocks(Test);
   test_automation(Test);
   test_reset(Test);
   test_reduction_meter(Test);
   test_extremes(Test);
   test_effect_integration(Test);
   report_throughput();
}

} // namespace audio_tests_audio_gate_dsp
