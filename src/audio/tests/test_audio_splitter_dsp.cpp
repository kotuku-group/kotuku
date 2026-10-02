// Included by audio.cpp to exercise the module implementation.

namespace audio_tests_audio_splitter_dsp {

using audio_tests_dsp_infrastructure::Delay;
using audio_tests_dsp_infrastructure::Feedback;

static std::span<const int> layout_for(int Channels)
{
   return Channels IS 2 ? std::span<const int>(glLayoutStereo) : std::span<const int>(glLayoutMono);
}

// A splitter in a top-level chain, initialised and edited through the same paths as a splitter object.

struct SplitterFixture {
   std::shared_ptr<std::recursive_mutex> Mutex = std::make_shared<std::recursive_mutex>();
   std::shared_ptr<AudioEffectChain> Chain = std::make_shared<AudioEffectChain>(Mutex);
   extAudioSplitter Splitter{nullptr, 100};
   int Rate, Channels;
   ERR Initialised;

   SplitterFixture(int pRate, int pChannels, double Budget,
      std::vector<SplitterBranchSettings> Branches = { { 0, false } }) : Rate(pRate), Channels(pChannels) {
      const auto layout = layout_for(Channels);
      Chain->Rate = Rate;
      Chain->Stereo = Channels IS 2;
      Chain->Layout.assign(layout.begin(), layout.end());
      Splitter.Chain = Chain;
      Splitter.Channel = 1 << 16;
      Splitter.OutputRate = Rate;
      Splitter.set_layout(layout);
      Splitter.Settings = std::move(Branches);
      Splitter.LatencyBudget = Budget;
      Chain->Effects.push_back(&Splitter);
      Initialised = AUDIOSPLITTER_Init(&Splitter);
   }

   std::shared_ptr<AudioEffectChain> branch(int Index) {
      std::lock_guard lock(*Mutex);
      return Splitter.Branches[Index];
   }

   ERR attach(extAudioEffect &Effect, int Index, std::unique_ptr<AudioEffectProcessor> Processor) {
      auto chain = branch(Index);
      Effect.Chain = chain;
      Effect.Branch = Index;
      Effect.Channel = 1 << 16;
      Effect.OutputRate = Rate;
      Effect.set_layout(layout_for(Channels));
      {
         std::lock_guard lock(*Mutex);
         Effect.Sequence = chain->NextSequence++;
         chain->Effects.push_back(&Effect);
         sort_effects(*chain);
      }
      return Effect.set_processor(std::move(Processor));
   }

   // Process every frame of Buffer through the top-level chain in blocks, as the mixer does.

   void process(std::vector<float> &Buffer, int Block = 512, size_t Start = 0, size_t End = SIZE_MAX) {
      End = std::min(End, Buffer.size() / Channels);
      for (size_t frame = Start; frame < End; frame += Block) {
         const int count = int(std::min(size_t(Block), End - frame));
         process_effects(*Chain, Buffer.data() + frame * Channels, count);
      }
   }

   template <class T> ERR edit(T &&Change) {
      AudioParamState state;
      effect_snapshot(&Splitter, state);
      Change(state.Groups[SPLITTER_BRANCHES]);
      return effect_commit(&Splitter, state);
   }

   ERR insert(int Index, double Gain, bool Mute) {
      return edit([&](auto &Entries) {
         Entries.insert(Entries.begin() + Index, AudioParamEntry { { Gain, Mute ? 1.0 : 0.0 }, -1 });
      });
   }

   ERR set(int Index, double Gain, bool Mute) {
      return edit([&](auto &Entries) { Entries[Index].Values = { Gain, Mute ? 1.0 : 0.0 }; });
   }

   ERR remove(int Index) {
      return edit([&](auto &Entries) { Entries.erase(Entries.begin() + Index); });
   }
};

static double noise(uint32_t &Seed)
{
   Seed = Seed * 1664525u + 1013904223u;
   return double(Seed >> 8) / double(1u << 23) - 1.0;
}

static std::vector<float> noise_buffer(int Frames, int Channels, double Level, uint32_t Seed)
{
   std::vector<float> buffer(size_t(Frames) * Channels);
   for (auto &sample : buffer) sample = float(noise(Seed) * Level);
   return buffer;
}

static std::vector<float> sine_buffer(int Frames, int Channels, int Rate, double Frequency, double Level)
{
   std::vector<float> buffer(size_t(Frames) * Channels);
   for (int i = 0; i < Frames; i++) {
      const float value = float(Level * std::sin(2.0 * std::numbers::pi * Frequency * i / Rate));
      for (int c = 0; c < Channels; c++) buffer[size_t(i) * Channels + c] = value;
   }
   return buffer;
}

// The greatest sample-to-sample change on the first channel between two frames.

static double max_step(const std::vector<float> &Buffer, int Channels, size_t Start, size_t End)
{
   double step = 0;
   for (size_t i = std::max(Start, size_t(1)); i < End; i++) {
      step = std::max(step, std::abs(double(Buffer[i * Channels]) - double(Buffer[(i - 1) * Channels])));
   }
   return step;
}

//********************************************************************************************************************
// A new splitter has one unmuted branch, has no latency and passes audio through bit-identically at any block size.

static void test_identity(AudioTestContext &Test)
{
   for (int rate : { 44100, 48000 }) {
      for (int channels = 1; channels <= 2; channels++) {
         for (int block : { 1, 7, 256, 1000 }) {
            SplitterFixture fixture(rate, channels, 0);
            AUDIO_REQUIRE(fixture.Initialised IS ERR::Okay and fixture.Splitter.Processor);
            AUDIO_CHECK(fixture.Splitter.CommittedLatency IS 0 and fixture.Splitter.latency() IS 0);
            const auto input = noise_buffer(3000, channels, 0.8, 7);
            auto output = input;
            fixture.process(output, block);
            AUDIO_CHECK(output IS input);
         }
      }
   }

   // An unconfigured splitter passes audio through.
   SplitterFixture idle(0, 2, 5);
   AUDIO_REQUIRE(idle.Initialised IS ERR::Okay);
   AUDIO_CHECK(idle.Splitter.CommittedLatency IS 0);
   float samples[] = { 0.25f, -0.5f, 2.0f, 1.0f };
   process_effects(*idle.Chain, samples, 2);
   AUDIO_CHECK(samples[0] IS 0.25f and samples[1] IS -0.5f and samples[2] IS 2.0f and samples[3] IS 1.0f);
}

//********************************************************************************************************************
// Unprocessed branches add without normalisation; muted branches are excluded and gains apply in decibels.

static void test_sum(AudioTestContext &Test)
{
   for (int branches = 2; branches <= SPLITTER_MAX_BRANCHES; branches++) {
      SplitterFixture fixture(48000, 2, 0, std::vector<SplitterBranchSettings>(branches, { 0, false }));
      AUDIO_REQUIRE(fixture.Initialised IS ERR::Okay);
      const auto input = noise_buffer(2000, 2, 0.5, 3);
      auto output = input;
      fixture.process(output);
      double error = 0;
      for (size_t i = 0; i < input.size(); i++) {
         error = std::max(error, std::abs(double(output[i]) - branches * double(input[i])));
      }
      AUDIO_CHECK(error < 1e-6);
   }

   SplitterFixture muted(48000, 2, 0, { { 0, false }, { 0, true }, { -6, true } });
   AUDIO_REQUIRE(muted.Initialised IS ERR::Okay);
   const auto input = noise_buffer(2000, 2, 0.5, 4);
   auto output = input;
   muted.process(output);
   AUDIO_CHECK(output IS input);

   SplitterFixture scaled(48000, 1, 0, { { -6, false }, { 12, false } });
   AUDIO_REQUIRE(scaled.Initialised IS ERR::Okay);
   output = { 0.25f, -0.125f };
   scaled.process(output);
   const double gain = std::pow(10.0, -6.0 / 20.0) + std::pow(10.0, 12.0 / 20.0);
   AUDIO_CHECK(std::abs(output[0] - 0.25 * gain) < 1e-6 and std::abs(output[1] + 0.125 * gain) < 1e-6);

   // The group is limited to between one and eight branches.
   AUDIO_CHECK(scaled.remove(0) IS ERR::Okay);
   AUDIO_CHECK(scaled.remove(0) IS ERR::InvalidValue);
   SplitterFixture full(48000, 1, 0, std::vector<SplitterBranchSettings>(SPLITTER_MAX_BRANCHES, { 0, false }));
   AUDIO_CHECK(full.insert(0, 0, true) IS ERR::InvalidValue);
}

//********************************************************************************************************************
// Branches with different latencies are realigned to the budget, so that their sum shows no comb filtering.

static void test_alignment(AudioTestContext &Test)
{
   for (auto [rate, expected] : { std::pair { 44100, 221 }, std::pair { 48000, 240 }, std::pair { 96000, 480 } }) {
      for (int channels = 1; channels <= 2; channels++) {
         for (int block : { 1, 300 }) {
            SplitterFixture fixture(rate, channels, 5, { { 0, false }, { 0, false } });
            AUDIO_REQUIRE(fixture.Initialised IS ERR::Okay);
            AUDIO_CHECK(fixture.Splitter.CommittedLatency IS expected);
            extAudioEffect limiter(nullptr, 1);
            AUDIO_REQUIRE(fixture.attach(limiter, 0, std::make_unique<LimiterProcessor>(&limiter,
               LimiterSettings())) IS ERR::Okay);
            AUDIO_CHECK(limiter.CommittedLatency IS expected);

            const auto input = noise_buffer(4 * expected, channels, 0.5, 11);
            auto output = input;
            fixture.process(output, block);
            bool aligned = true;
            for (size_t i = 0; i < output.size(); i++) {
               const size_t offset = size_t(expected) * channels;
               const float wanted = (i < offset) ? 0.0f : input[i - offset] + input[i - offset];
               aligned &= output[i] IS wanted;
            }
            AUDIO_CHECK(aligned);
         }
      }
   }

   // A branch with a different latency is delayed by the remainder of the budget: 3 ms is 144 frames at 48 kHz.
   SplitterFixture fixture(48000, 1, 3, { { 0, false }, { 0, false } });
   AUDIO_REQUIRE(fixture.Initialised IS ERR::Okay);
   extAudioEffect delay(nullptr, 1);
   AUDIO_REQUIRE(fixture.attach(delay, 1, std::make_unique<Delay>(100, 1)) IS ERR::Okay);
   std::vector<float> output(400, 0.0f);
   output[0] = 1;
   fixture.process(output, 64);
   for (size_t i = 0; i < output.size(); i++) AUDIO_CHECK(output[i] IS (i IS 144 ? 2.0f : 0.0f));
}

//********************************************************************************************************************
// Attachment fails if a branch would exceed the latency budget, counting bypassed effects.

static void test_budget(AudioTestContext &Test)
{
   auto limiter = [](extAudioEffect &Effect) {
      return std::make_unique<LimiterProcessor>(&Effect, LimiterSettings());
   };

   SplitterFixture none(48000, 2, 0);
   extAudioEffect first(nullptr, 1);
   AUDIO_CHECK(none.attach(first, 0, limiter(first)) IS ERR::OutOfRange);
   AUDIO_CHECK(!first.processor and first.CommittedLatency IS 0);

   SplitterFixture short_budget(48000, 2, 4.9); // 236 frames
   extAudioEffect second(nullptr, 2);
   AUDIO_CHECK(short_budget.attach(second, 0, limiter(second)) IS ERR::OutOfRange);

   SplitterFixture fixture(48000, 2, 5, { { 0, false }, { 0, false } });
   extAudioEffect a(nullptr, 3), b(nullptr, 4), c(nullptr, 5);
   AUDIO_CHECK(fixture.attach(a, 0, limiter(a)) IS ERR::Okay);
   a.Flags = AEF::BYPASS; // Bypass can be cleared at any time, so it does not release budget.
   AUDIO_CHECK(fixture.attach(b, 0, limiter(b)) IS ERR::OutOfRange);
   AUDIO_CHECK(fixture.attach(c, 1, limiter(c)) IS ERR::Okay); // Each branch has the full budget.
   a.Flags = AEF::NIL;

   SplitterFixture wide(48000, 2, 10);
   extAudioEffect d(nullptr, 6), e(nullptr, 7);
   AUDIO_CHECK(wide.attach(d, 0, limiter(d)) IS ERR::Okay and wide.attach(e, 0, limiter(e)) IS ERR::Okay);
   AUDIO_CHECK(wide.Splitter.CommittedLatency IS 480);
}

//********************************************************************************************************************
// Bypass changes the branch latency, and the branch moves to its new alignment with a crossfade.  Leaving bypass is
// smooth.  Entering bypass discards the audio buffered inside the bypassed effect, exactly as in a top-level chain,
// so only the realignment and the bounded level of the transition are tested.

static void test_bypass_realignment(AudioTestContext &Test)
{
   const int rate = 48000, lag = 240, fade = 480;
   SplitterFixture fixture(rate, 2, 5, { { 0, false }, { 0, false } });
   AUDIO_REQUIRE(fixture.Initialised IS ERR::Okay);
   extAudioEffect limiter(nullptr, 1);
   AUDIO_REQUIRE(fixture.attach(limiter, 0, std::make_unique<LimiterProcessor>(&limiter, LimiterSettings())) IS
      ERR::Okay);

   const auto input = sine_buffer(rate / 2, 2, rate, 220, 0.4);
   auto output = input;
   auto aligned = [&](size_t Start, size_t End) {
      bool result = true;
      for (size_t i = Start * 2; i < End * 2; i++) result &= output[i] IS input[i - lag * 2] + input[i - lag * 2];
      return result;
   };

   fixture.process(output, 64, 0, 4800);
   AUDIO_CHECK(aligned(lag, 4800));
   const double steady = max_step(output, 2, lag + 1, 4800);

   AUDIO_CHECK(AUDIOEFFECT_SET_Flags(&limiter, AEF::BYPASS) IS ERR::Okay);
   fixture.process(output, 64, 4800, 9600);
   double peak = 0;
   for (size_t i = 4800 * 2; i < 9600 * 2; i++) peak = std::max(peak, std::abs(double(output[i])));
   AUDIO_CHECK(peak <= 0.8 + 1e-6);
   AUDIO_CHECK(aligned(4800 + fade + 64, 9600));

   AUDIO_CHECK(AUDIOEFFECT_SET_Flags(&limiter, AEF::NIL) IS ERR::Okay);
   fixture.process(output, 64, 9600, 14400);
   AUDIO_CHECK(max_step(output, 2, 9600, 9600 + fade + 64) <= steady * 1.5);
   AUDIO_CHECK(aligned(9600 + fade + 64, 14400));
}

//********************************************************************************************************************
// Gain and mute changes ramp over 10 ms.  Inserted branches are inaudible until enabled and ramp in.  Retained branches
// keep their effect state and follow their entry; removed branches fade out and disconnect their effects.

static void test_live_edits(AudioTestContext &Test)
{
   const int rate = 48000, fade = 480;
   SplitterFixture fixture(rate, 1, 0, { { 0, false }, { 0, false } });
   AUDIO_REQUIRE(fixture.Initialised IS ERR::Okay);
   std::vector<float> output(rate, 0.25f);

   fixture.process(output, 100, 0, 1000);
   AUDIO_CHECK(output[999] IS 0.5f);
   AUDIO_REQUIRE(fixture.set(1, -12, false) IS ERR::Okay);
   fixture.process(output, 100, 1000, 3000);
   const double target = 0.25 * (1 + std::pow(10.0, -12.0 / 20.0));
   const double ramp_step = (0.5 - target) / fade;
   AUDIO_CHECK(std::abs(output[1000 + fade + 10] - target) < 1e-6);
   AUDIO_CHECK(max_step(output, 1, 1000, 3000) <= ramp_step * 1.01);
   bool monotonic = true;
   for (int i = 1001; i < 1000 + fade; i++) monotonic &= output[i] <= output[i - 1];
   AUDIO_CHECK(monotonic and output[1000 + fade / 2] < 0.5f and output[1000 + fade / 2] > float(target));

   AUDIO_REQUIRE(fixture.set(1, -12, true) IS ERR::Okay);
   fixture.process(output, 100, 3000, 4000);
   AUDIO_CHECK(output[3999] IS 0.25f and max_step(output, 1, 3000, 4000) <= ramp_step * 1.01);

   // An integrator in branch 1 shows whether its state survives branch edits.
   AUDIO_REQUIRE(fixture.set(1, 0, false) IS ERR::Okay);
   extAudioEffect integrator(nullptr, 1);
   AUDIO_REQUIRE(fixture.attach(integrator, 1, std::make_unique<Feedback>()) IS ERR::Okay);
   std::fill(output.begin(), output.end(), 0.001f);
   fixture.process(output, 100, 4000, 6000);
   const float slope = output[5999] - output[5998];
   AUDIO_CHECK(std::abs(slope - 0.001f) < 1e-6);

   auto integrator_chain = fixture.branch(1);
   AUDIO_REQUIRE(fixture.insert(0, 0, true) IS ERR::Okay); // Inserted muted: inaudible
   AUDIO_CHECK(fixture.branch(2) IS integrator_chain and integrator_chain->Branch IS 2 and integrator.Branch IS 2);
   fixture.process(output, 100, 6000, 7000);
   AUDIO_CHECK(std::abs((output[6000] - output[5999]) - slope) < 1e-5);
   AUDIO_CHECK(std::abs((output[6999] - output[6998]) - slope) < 1e-5);

   // Enabling the inserted branch ramps it in.
   AUDIO_REQUIRE(fixture.set(0, 0, false) IS ERR::Okay);
   fixture.process(output, 100, 7000, 8000);
   AUDIO_CHECK(max_step(output, 1, 7000, 8000) <= 0.001 / fade * 1.01 + 0.0011);
   AUDIO_CHECK(std::abs((output[7999] - output[7998]) - slope) < 1e-5);

   // Removing the audible integrator branch fades it out while its captured processor keeps running.
   const float before = output[7999];
   integrator_chain.reset();
   AUDIO_REQUIRE(fixture.remove(2) IS ERR::Okay);
   AUDIO_CHECK(fixture.Splitter.Processor->retiring() IS 1);
   AUDIO_CHECK(integrator.Chain.expired()); // Disconnected immediately
   fixture.process(output, 100, 8000, 9000);
   AUDIO_CHECK(fixture.Splitter.Processor->retiring() IS 0);
   AUDIO_CHECK(std::abs(output[8000] - before) < 0.01f);
   AUDIO_CHECK(max_step(output, 1, 8000, 8000 + fade) <= before / fade * 1.1 + 0.0011);
   AUDIO_CHECK(output[8999] IS 0.002f); // Two dry branches remain

   // A later edit releases the finished slot, while the newly removed audible branch fades out.
   AUDIO_REQUIRE(fixture.remove(0) IS ERR::Okay);
   AUDIO_CHECK(fixture.Splitter.Processor->branch_count() IS 1 and fixture.Splitter.Processor->retiring() IS 1);
   fixture.process(output, 100, 9000, 10000);
   AUDIO_CHECK(fixture.Splitter.Processor->retiring() IS 0 and output[9999] IS 0.001f);
}

//********************************************************************************************************************
// Branch tails keep the parent chain draining.  The tail bound and decay estimate cover a branch reverb, MaxDrain
// still truncates it, and muted branches do not hold the drain.

static void test_drain(AudioTestContext &Test)
{
   const int rate = 48000;
   auto drain = [&](SplitterFixture &Fixture, uint64_t Limit, uint64_t &Bound, int &Audible) {
      auto &chain = *Fixture.Chain;
      std::vector<float> block(512 * 2, 0.0f);
      block[0] = block[1] = 1;
      render_effects(chain, block.data(), 512, 1, Limit);
      Bound = chain.tail_bound();
      int frames = 512;
      Audible = 0;
      while ((chain.State != ADS::IDLE) and (frames < rate * 20)) {
         std::fill(block.begin(), block.end(), 0.0f);
         render_effects(chain, block.data(), 512, 0, Limit);
         for (int i = 0; i < 512; i++) {
            if (block[i * 2] != 0) Audible = frames + i;
         }
         frames += 512;
      }
      return frames;
   };

   SplitterFixture fixture(rate, 2, 0, { { 0, false }, { 0, false } });
   extAudioEffect reverb(nullptr, 1);
   AUDIO_REQUIRE(fixture.attach(reverb, 0, std::make_unique<ReverbProcessor>(&reverb, ReverbSettings())) IS
      ERR::Okay);
   uint64_t bound;
   int audible;
   const int frames = drain(fixture, UINT64_MAX, bound, audible);
   AUDIO_CHECK(fixture.Chain->State IS ADS::IDLE and !fixture.Chain->Truncated);
   AUDIO_CHECK(audible > rate); // A 1.5 second reverb tail drained through the splitter
   AUDIO_CHECK(bound >= uint64_t(audible) and frames < rate * 20);
   AUDIO_CHECK(fixture.Splitter.processor->decay_estimate() IS reverb.processor->decay_estimate());
   AUDIO_CHECK(fixture.Chain->decay_estimate() >= reverb.processor->decay_estimate());
   AUDIO_CHECK((reverb.Meter.Flags & AMF::IDLE) != AMF::NIL);

   SplitterFixture truncated(rate, 2, 0, { { 0, false }, { 0, false } });
   extAudioEffect reverb2(nullptr, 2);
   AUDIO_REQUIRE(truncated.attach(reverb2, 0, std::make_unique<ReverbProcessor>(&reverb2, ReverbSettings())) IS
      ERR::Okay);
   drain(truncated, 4800, bound, audible);
   AUDIO_CHECK(truncated.Chain->Truncated and audible < 4800 + 512);

   SplitterFixture muted(rate, 2, 0, { { 0, true }, { 0, false } });
   extAudioEffect reverb3(nullptr, 3);
   AUDIO_REQUIRE(muted.attach(reverb3, 0, std::make_unique<ReverbProcessor>(&reverb3, ReverbSettings())) IS
      ERR::Okay);
   AUDIO_CHECK(drain(muted, UINT64_MAX, bound, audible) IS 512); // Idle as soon as the source stops

   // Alignment storage drains: an empty branch with a 5 ms budget is pending until its delayed input has emerged.
   SplitterFixture aligned(rate, 2, 5);
   AUDIO_CHECK(aligned.Splitter.processor->tail_frames() IS 0); // Inactive until the first reset
   std::vector<float> block(200 * 2, 0.0f);
   block[0] = 1;
   render_effects(*aligned.Chain, block.data(), 200, 1, UINT64_MAX);
   AUDIO_CHECK(effects_pending(*aligned.Chain) and aligned.Splitter.processor->tail_frames() IS 240);
   std::fill(block.begin(), block.end(), 0.0f);
   render_effects(*aligned.Chain, block.data(), 200, 0, UINT64_MAX);
   AUDIO_CHECK(block[40 * 2] IS 1.0f and !effects_pending(*aligned.Chain));
}

//********************************************************************************************************************
// Output is independent of the block size, including across a live edit, and stereo channels remain isolated.

static void test_blocks_and_channels(AudioTestContext &Test)
{
   const int rate = 48000;
   std::vector<float> reference;
   for (int block : { 1, 64, 300, 4096 }) {
      SplitterFixture fixture(rate, 2, 5, { { -3, false }, { 0, false } });
      extAudioEffect reverb(nullptr, 1), limiter(nullptr, 2);
      AUDIO_REQUIRE(fixture.attach(reverb, 0, std::make_unique<ReverbProcessor>(&reverb, ReverbSettings())) IS
         ERR::Okay);
      AUDIO_REQUIRE(fixture.attach(limiter, 1, std::make_unique<LimiterProcessor>(&limiter, LimiterSettings())) IS
         ERR::Okay);
      auto output = noise_buffer(9000, 2, 0.7, 21);
      fixture.process(output, block, 0, 3000);
      AUDIO_REQUIRE(fixture.set(1, -6, false) IS ERR::Okay);
      fixture.process(output, block, 3000, 9000);
      if (reference.empty()) reference = output;
      else AUDIO_CHECK(output IS reference);
   }

   SplitterFixture fixture(rate, 2, 5, { { 0, false }, { 0, false } });
   extAudioEffect limiter(nullptr, 1);
   AUDIO_REQUIRE(fixture.attach(limiter, 0, std::make_unique<LimiterProcessor>(&limiter, LimiterSettings())) IS
      ERR::Okay);
   auto output = noise_buffer(4000, 2, 0.5, 5);
   for (size_t i = 1; i < output.size(); i += 2) output[i] = 0;
   fixture.process(output);
   bool silent = true;
   for (size_t i = 1; i < output.size(); i += 2) silent &= output[i] IS 0;
   AUDIO_CHECK(silent);
}

//********************************************************************************************************************
// Chain resets reach branch effects, idling the container idles them, and skipped frames are forwarded.

static void test_lifecycle(AudioTestContext &Test)
{
   class Counter final : public AudioEffectProcessor {
   public:
      int Resets = 0, Skipped = 0;
      void process(float *, int) override { }
      void reset() override { ++Resets; }
      void skip(int Frames) override { Skipped += Frames; }
   };

   SplitterFixture fixture(48000, 2, 0);
   extAudioEffect effect(nullptr, 1);
   auto counter = std::make_unique<Counter>();
   auto probe = counter.get();
   AUDIO_REQUIRE(fixture.attach(effect, 0, std::move(counter)) IS ERR::Okay);
   std::vector<float> output(512 * 2, 0.25f);
   fixture.process(output);
   AUDIO_CHECK(probe->Resets >= 1 and !effect.ResetPending);

   fixture.Chain->reset();
   AUDIO_CHECK(effect.ResetPending and fixture.Splitter.ResetPending);
   const int resets = probe->Resets;
   fixture.process(output);
   AUDIO_CHECK(probe->Resets > resets and !effect.ResetPending);

   AUDIO_CHECK(effect.MeterFrames > 0);
   fixture.Splitter.idle();
   AUDIO_CHECK((effect.Meter.Flags & AMF::IDLE) != AMF::NIL and effect.MeterFrames IS 0);

   fixture.Splitter.Flags = AEF::BYPASS;
   fixture.process(output);
   AUDIO_CHECK(probe->Skipped IS 512);
   AUDIO_CHECK(output[0] IS 0.25f);
}

//********************************************************************************************************************
// Activation configures branch chains with their container and reports any budget excess.  Closing the channel set
// disconnects the branch effects.

static void test_commit(AudioTestContext &Test)
{
   extAudio *audio;
   AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
   std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
   AUDIO_REQUIRE(InitObject(audio) IS ERR::Okay);
   audio->Sets.resize(2);
   auto &set = audio->Sets[1];
   set.Effects = std::make_shared<AudioEffectChain>(audio->MixerLock);
   set.Effects->Generation = audio->GlobalEffects->Generation;

   extAudioSplitter splitter(nullptr, 101);
   extAudioEffect inner(nullptr, 102);
   splitter.Chain = set.Effects;
   splitter.Channel = 1 << 16;
   splitter.LatencyBudget = 1;
   set.Effects->Effects.push_back(&splitter);
   AUDIO_REQUIRE(AUDIOSPLITTER_Init(&splitter) IS ERR::Okay);
   AUDIO_CHECK(splitter.Branches.size() IS 1 and splitter.Branches[0]->Container IS splitter.UID);
   AUDIO_CHECK(splitter.Branches[0]->Generation IS set.Effects->Generation);

   // The rate is unknown, so the limiter's latency cannot yet be checked against the 1 ms budget.
   inner.Chain = splitter.Branches[0];
   splitter.Branches[0]->Effects.push_back(&inner);
   AUDIO_REQUIRE(inner.set_processor(std::make_unique<LimiterProcessor>(&inner, LimiterSettings())) IS ERR::Okay);
   AUDIO_CHECK(inner.CommittedLatency IS 0);

   audio->OutputRate = 48000;
   AUDIO_REQUIRE(commit_audio_output(audio, glLayoutStereo, nullptr) IS ERR::Okay);
   AUDIO_CHECK(inner.OutputRate IS 48000 and inner.FormatCommitted and inner.Layout.size() IS 2);
   AUDIO_CHECK(inner.FormatGeneration IS audio->OutputGeneration);
   AUDIO_CHECK(splitter.Branches[0]->Rate IS 48000 and splitter.Branches[0]->Layout.size() IS 2);
   AUDIO_CHECK(inner.CommittedLatency IS 240 and splitter.CommittedLatency IS 48);

   int64_t latency;
   AUDIO_CHECK(set.Effects->latency(latency) IS ERR::Okay and latency IS 48);

   set.Effects.reset();
   AUDIO_CHECK(inner.Chain.expired() and splitter.Branches.empty());
   AUDIO_CHECK(acDeactivate(audio) IS ERR::Okay);
}

//********************************************************************************************************************

static void run(AudioTestContext &Test)
{
   test_identity(Test);
   test_sum(Test);
   test_alignment(Test);
   test_budget(Test);
   test_bypass_realignment(Test);
   test_live_edits(Test);
   test_drain(Test);
   test_blocks_and_channels(Test);
   test_lifecycle(Test);
   test_commit(Test);
}

} // namespace audio_tests_audio_splitter_dsp
