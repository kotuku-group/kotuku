// Deterministic processors exercise infrastructure without adding production effect classes.
namespace audio_tests_dsp_infrastructure {

class Delay final : public AudioEffectProcessor {
   std::vector<float> storage;
   size_t cursor = 0, occupied = 0;
   int channels;
   int frames;
public:
   int Resets = 0;
   Delay(int Frames, int Channels) : storage(Frames * Channels), channels(Channels), frames(Frames) { }
   AudioTail tail() const override { return AudioTail::FINITE; }
   uint64_t tail_frames() const override { return frames; }
   int64_t latency() const override { return frames; }
   bool pending() const override { return occupied != 0; }
   void reset() override { std::fill(storage.begin(), storage.end(), 0); cursor = occupied = 0; ++Resets; }
   void process(float *Buffer, int Frames) override {
      for (int i = 0; i < Frames * channels; ++i) {
         const float input = Buffer[i];
         Buffer[i] = storage[cursor];
         if (storage[cursor] != 0) --occupied;
         storage[cursor] = input;
         if (input != 0) ++occupied;
         cursor = (cursor + 1) % storage.size();
      }
   }
};

class Feedback : public AudioEffectProcessor {
   float value = 0;
public:
   AudioTail tail() const override { return AudioTail::INDEFINITE; }
   bool pending() const override { return value != 0; }
   double gain_reduction() const override { return 3; }
   void reset() override { value = 0; }
   void process(float *Buffer, int Frames) override {
      for (int i = 0; i < Frames; ++i) {
         value += Buffer[i];
         Buffer[i] = value;
      }
   }
};

struct Fixture {
   std::shared_ptr<std::recursive_mutex> Mutex = std::make_shared<std::recursive_mutex>();
   std::shared_ptr<AudioEffectChain> Chain = std::make_shared<AudioEffectChain>(Mutex);
   extAudioEffect Effect{nullptr, 0};
   Fixture(int Rate, int Channels, std::unique_ptr<AudioEffectProcessor> Processor) {
      Effect.Chain = Chain;
      Effect.OutputRate = Chain->Rate = Rate;
      Effect.Stereo = Chain->Stereo = Channels IS 2;
      Chain->Effects.push_back(&Effect);
      if (Processor) Effect.set_processor(std::move(Processor));
   }
};

static void delays(AudioTestContext &Test)
{
   for (int rate : {44100, 48000, 96000}) for (int channels : {1, 2}) {
      Fixture fixture(rate, channels, std::make_unique<Delay>(107, channels));
      auto &chain = *fixture.Chain;
      extAudioEffect second(nullptr, 0);
      second.Chain = fixture.Chain;
      second.OutputRate = rate;
      second.Stereo = channels IS 2;
      chain.Effects.push_back(&second);
      AUDIO_REQUIRE(second.set_processor(std::make_unique<Delay>(71, channels)) IS ERR::Okay);
      int64_t latency;
      AUDIO_CHECK(chain.latency(latency) IS ERR::Okay and latency IS 178);
      AUDIO_CHECK(!effects_pending(chain));
      std::array<float, 2> frame{};
      for (int i = 0; i < 240; ++i) {
         frame = {};
         if (!i) frame[0] = 1;
         render_effects(chain, frame.data(), 1, i IS 0 ? 1 : 0, rate);
         AUDIO_CHECK(frame[0] IS (i IS 178 ? 1.0f : 0.0f));
         if (channels IS 2) AUDIO_CHECK(frame[1] IS 0);
         if (i < 178) AUDIO_CHECK(effects_pending(chain));
      }
      AUDIO_CHECK(chain.State IS ADS::IDLE and !effects_pending(chain) and !chain.Truncated);
      AUDIO_CHECK((fixture.Effect.Meter.Flags & AMF::IDLE) != AMF::NIL);
      second.Flags = AEF::BYPASS;
      AUDIO_CHECK(chain.latency(latency) IS ERR::Okay and latency IS 107);
      second.detach();
      AUDIO_CHECK(chain.latency(latency) IS ERR::Okay and latency IS 107);
      fixture.Effect.CommittedLatency = INT64_MAX;
      second.Chain = fixture.Chain;
      second.Flags = AEF::NIL;
      chain.Effects.push_back(&second);
      AUDIO_CHECK(chain.latency(latency) IS ERR::OutOfRange);
   }
}

static void deadlines(AudioTestContext &Test)
{
   Fixture fixture(1000, 1, std::make_unique<Feedback>());
   auto &chain = *fixture.Chain;
   float value = 1;
   render_effects(chain, &value, 1, 1, 100);
   AUDIO_CHECK(effects_pending(chain));
   std::array<float, 117> tail{};
   render_effects(chain, tail.data(), tail.size(), 0, 100);
   for (int i = 0; i < 90; ++i) AUDIO_CHECK(tail[i] IS 1);
   for (int i = 90; i < 100; ++i) AUDIO_CHECK(std::abs(tail[i] - (99 - i) / 10.0f) < 1e-6);
   for (int i = 100; i < 117; ++i) AUDIO_CHECK(tail[i] IS 0);
   AUDIO_CHECK(chain.Truncated and !effects_pending(chain) and chain.State IS ADS::IDLE);
   value = 1;
   render_effects(chain, &value, 1, 1, 100);
   AUDIO_CHECK(value IS 1 and !chain.Truncated);
   value = 0;
   render_effects(chain, &value, 1, 0, 100);
   value = 1;
   render_effects(chain, &value, 1, 1, 100);
   AUDIO_CHECK(value IS 2 and chain.DrainFrames IS 0); // New source retains history.
   fixture.Effect.Channel = 1 << 16;
   AUDIO_CHECK(AUDIOEFFECT_SET_Flags(&fixture.Effect, AEF::BYPASS) IS ERR::Okay);
   AUDIO_CHECK(!effects_pending(chain));
   AUDIO_CHECK(AUDIOEFFECT_SET_Flags(&fixture.Effect, AEF::NIL) IS ERR::Okay);
   value = 0;
   render_effects(chain, &value, 1, 1, 100);
   AUDIO_CHECK(value IS 0 and !effects_pending(chain));
   chain.reset();
   AUDIO_CHECK(!effects_pending(chain));
   value = 1;
   render_effects(chain, &value, 1, 1, 100);
   AUDIO_CHECK(value IS 1);

   class Faulty final : public Feedback {
   public:
      AudioTail tail() const override { return AudioTail::FINITE; }
      uint64_t tail_frames() const override { return 0; } // Deliberately false: the watchdog must still stop it.
   };
   Fixture faulty(1000, 1, std::make_unique<Faulty>());
   value = 1;
   render_effects(*faulty.Chain, &value, 1, 1, 100);
   for (int i = 0; i < 110; ++i) {
      value = 0;
      render_effects(*faulty.Chain, &value, 1, 0, 100);
      const float expected = i < 90 ? 1.0f : (i < 100 ? (99 - i) / 10.0f : 0.0f);
      AUDIO_CHECK(std::abs(value - expected) < 1e-6);
   }
   AUDIO_CHECK(faulty.Chain->Truncated and !effects_pending(*faulty.Chain));

   Fixture finite(1000, 1, std::make_unique<Delay>(95, 1));
   value = 1;
   render_effects(*finite.Chain, &value, 1, 1, 100);
   for (int i = 1; i <= 100; ++i) {
      value = 0;
      render_effects(*finite.Chain, &value, 1, 0, 100);
      AUDIO_CHECK(value IS (i IS 95 ? 1.0f : 0.0f)); // Finite completion inside the fade window is not faded.
   }
   AUDIO_CHECK(!finite.Chain->Truncated);

   Fixture untouched(1000, 1, std::make_unique<Feedback>());
   value = 0;
   render_effects(*untouched.Chain, &value, 1, 0, 100);
   AUDIO_CHECK(!effects_pending(*untouched.Chain) and untouched.Effect.Meter.Sequence IS 0);
}

static void upstream_silence(AudioTestContext &Test)
{
   class Clock final : public AudioEffectProcessor {
   public:
      int Frames = 0, Resets = 0;
      void reset() override { Frames = 0; ++Resets; }
      void process(float *Buffer, int Count) override { Frames += Count; }
   };

   auto processor = std::make_unique<Clock>();
   auto clock = processor.get();
   Fixture fixture(1000, 1, std::move(processor));
   std::array<float, 17> silence{};
   render_effects(*fixture.Chain, silence.data(), silence.size(), 0, 1000, false, 107, true);
   AUDIO_CHECK(clock->Frames IS 17 and clock->Resets IS 1);
   AUDIO_CHECK(fixture.Chain->State IS ADS::DRAINING);
}

static void meters(AudioTestContext &Test)
{
   Fixture fixture(44100, 2, std::make_unique<Delay>(1, 2));
   auto &effect = fixture.Effect;
   std::vector<float> samples(2205 * 2, 0.5f);
   for (size_t i = 1; i < samples.size(); i += 2) samples[i] = 0.25f;
   effect.process(samples.data(), 1000);
   AUDIO_CHECK(effect.Meter.Sequence IS 0);
   effect.process(samples.data() + 2000, 1205);
   const auto snapshot = effect_meters(&effect);
   AUDIO_CHECK(snapshot.Sequence IS 1 and snapshot.Interval IS 2205 and snapshot.Position IS 2205);
   AUDIO_CHECK(std::abs(snapshot.Values[0] + 6.020599913) < 1e-6);
   AUDIO_CHECK(std::abs(snapshot.Values[1] + 12.041199827) < 1e-6);
   AUDIO_CHECK(snapshot.Flags IS AMF::VALID and !snapshot.Floor);
   AUDIO_CHECK(effect_meters(&effect).Sequence IS snapshot.Sequence);
   std::array<float, 20> silence{};
   effect.process(silence.data(), 10);
   effect.idle();
   AUDIO_CHECK(effect.Meter.Sequence IS 2 and effect.Meter.Interval IS 10 and
      effect.Meter.Flags IS (AMF::VALID|AMF::IDLE));
   AUDIO_CHECK(effect.Meter.Values[0] IS -120 and (effect.Meter.Floor & 3) IS 3);
   fixture.Chain->reset();
   AUDIO_CHECK((effect_meters(&effect).Flags & AMF::NO_SAMPLES) != AMF::NIL and effect.Meter.Interval IS 0);
   effect.Flags = AEF::BYPASS;
   AUDIO_CHECK(effect_meters(&effect).Flags IS AMF::BYPASSED);
   effect.Flags = AEF::NIL;
   effect.detach();
   AUDIO_CHECK((effect_meters(&effect).Flags & AMF::VALID) IS AMF::NIL);

   Fixture integer_output(1000, 1, std::make_unique<Delay>(1, 1));
   integer_output.Chain->MeterScale = 32768; // Both integer output formats use 16-bit internal mixer units.
   std::array<float, 50> integer_samples;
   std::fill(integer_samples.begin(), integer_samples.end(), 16384);
   integer_output.Effect.process(integer_samples.data(), integer_samples.size());
   AUDIO_CHECK(std::abs(integer_output.Effect.Meter.Values[0] + 6.020599913) < 1e-6);

   Fixture reduction(1000, 1, std::make_unique<Feedback>());
   std::array<float, 50> input{};
   input[0] = 1;
   reduction.Effect.process(input.data(), input.size());
   AUDIO_CHECK(reduction.Effect.Meter.Values[4] IS 3);
}

static void equaliser_tail(AudioTestContext &Test)
{
   Fixture fixture(48000, 1, nullptr);
   fixture.Effect.processor = std::make_shared<EqualiserProcessor>(&fixture.Effect,
      std::vector<AudioEQBand>{{EQB::LOW_PASS, 1000, 0, 0.707}}, 0);
   extAudioEffect delay(nullptr, 0);
   delay.Chain = fixture.Chain;
   delay.OutputRate = 48000;
   fixture.Chain->Effects.insert(fixture.Chain->Effects.begin(), &delay);
   AUDIO_REQUIRE(delay.set_processor(std::make_unique<Delay>(107, 1)) IS ERR::Okay);
   float sample = 1;
   render_effects(*fixture.Chain, &sample, 1, 1, 48000);
   AUDIO_CHECK(effects_pending(*fixture.Chain));
   int frames = 0;
   while (effects_pending(*fixture.Chain) and frames < 48000) {
      sample = 0;
      render_effects(*fixture.Chain, &sample, 1, 0, 48000);
      ++frames;
   }
   AUDIO_CHECK(frames > 117 and frames < 48000 and !fixture.Chain->Truncated);
   sample = 0;
   render_effects(*fixture.Chain, &sample, 1, 1, 48000);
   AUDIO_CHECK(sample IS 0); // Residual history must never reappear.
}

static void contention(AudioTestContext &Test)
{
   Fixture fixture(48000, 2, std::make_unique<Delay>(128, 2));
   std::atomic<bool> done{false};
   std::atomic<uint64_t> reads{0};
   auto reader = std::thread([&] {
      uint64_t sequence = 0;
      while (!done) {
         auto snapshot = effect_meters(&fixture.Effect);
         AUDIO_CHECK(snapshot.Sequence >= sequence);
         sequence = snapshot.Sequence;
         ++reads;
      }
   });
   std::array<float, 512> buffer{};
   double max_wait = 0, max_render = 0;
   const auto start = std::chrono::steady_clock::now();
   for (int i = 0; i < 2000; ++i) {
      std::fill(buffer.begin(), buffer.end(), 0.25f);
      const auto waiting = std::chrono::steady_clock::now();
      std::lock_guard lock(*fixture.Mutex);
      const auto acquired = std::chrono::steady_clock::now();
      render_effects(*fixture.Chain, buffer.data(), 256, 256, 48000, true);
      const auto rendered = std::chrono::steady_clock::now();
      max_wait = std::max(max_wait, std::chrono::duration<double>(acquired - waiting).count());
      max_render = std::max(max_render, std::chrono::duration<double>(rendered - acquired).count());
   }
   const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
   done = true;
   reader.join();
   AUDIO_CHECK(reads > 0);
   kt::Log("DSP headroom").msg("512000 stereo frames with continuous polling: %.6f seconds, %llu reads.",
      elapsed, (unsigned long long)reads.load());
   kt::Log("DSP headroom").msg("Maximum lock wait %.3f us, render %.3f us; period budget 5333.333 us.",
      max_wait * 1e6, max_render * 1e6);
}

#ifdef AUDIO_WORKER
static void common_mixer(AudioTestContext &Test)
{
   extAudio *audio;
   AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
   std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
   AUDIO_REQUIRE(InitObject(audio) IS ERR::Okay);
   audio->Flags = ADF::NIL;
   audio->OutputRate = 1000;
   audio->BitDepth = 32;
   audio->DriverBitSize = 4;
   audio->Stereo = false;
   audio->MixElements = SAMPLE(17);
   audio->MixBuffer.resize(17);
   audio->MixConfig = AudioConfig(false, false);
   audio->Samples.resize(2);
   auto &sample = audio->Samples[1];
   sample.SampleType = SFM::S16_BIT_MONO;
   sample.SampleLength = SAMPLE(1);
   sample.Data.resize(4);
   const int16_t impulse = 32767;
   std::memcpy(sample.Data.data(), &impulse, sizeof(impulse));
   audio->Sets.resize(2);
   auto &set = audio->Sets[1];
   set.Channel.resize(1);
   auto &channel = set.Channel[0];
   channel.SampleHandle = 1;
   channel.Frequency = 1000;
   channel.State = CHS::PLAYING;
   channel.LVolume = channel.RVolume = 1;
   channel.Handle = 1 << 16;
   set.Effects = std::make_shared<AudioEffectChain>(audio->MixerLock);
   set.Effects->Generation = audio->GlobalEffects->Generation;
   set.ScratchBuffer.resize(17);
   extAudioEffect application(nullptr, 0), global(nullptr, 0);
   application.Chain = set.Effects;
   global.Chain = audio->GlobalEffects;
   application.OutputRate = global.OutputRate = 1000;
   set.Effects->Effects.push_back(&application);
   audio->GlobalEffects->Effects.push_back(&global);
   set.Effects->Rate = audio->GlobalEffects->Rate = 1000;
   AUDIO_REQUIRE(application.set_processor(std::make_unique<Delay>(107, 1)) IS ERR::Okay);
   auto global_processor = std::make_unique<Delay>(71, 1);
   auto global_delay = global_processor.get();
   AUDIO_REQUIRE(global.set_processor(std::move(global_processor)) IS ERR::Okay);
   AUDIO_CHECK(audio_playing(audio));
   std::array<float, 17> output{};
   std::vector<float> rendered;
   audio->EffectConfigured = true;
   snd::GetEffectStatus status{};
   status.Channel = 1 << 16;
   bool global_only = false, upstream_silence = false;
   for (int period = 0; period < 20 and audio_playing(audio); ++period) {
      mix_data(audio, 17, output.data());
      rendered.insert(rendered.end(), output.begin(), output.end());
      if (application.pending() and !global.pending() and channel.isStopped()) {
         upstream_silence = true;
         AUDIO_CHECK(global_delay->Resets IS 1);
      }
      if (!application.pending() and global.pending() and channel.isStopped()) {
         global_only = true;
         AUDIO_CHECK(AUDIO_GetEffectStatus(audio, &status) IS ERR::Okay and status.State IS ADS::DRAINING);
      }
   }
   AUDIO_REQUIRE(rendered.size() > 178);
   for (size_t i = 0; i < rendered.size(); ++i) AUDIO_CHECK(rendered[i] IS (i IS 178 ? 1.0f : 0.0f));
   AUDIO_CHECK(upstream_silence and global_only and !audio_playing(audio));
   AUDIO_CHECK(AUDIO_GetEffectStatus(audio, &status) IS ERR::Okay);
   AUDIO_CHECK(status.Application IS 107 and status.Global IS 71 and status.Total IS 178);
   AUDIO_CHECK(status.State IS ADS::IDLE);
   audio->GlobalEffects->Truncated = true;
   AUDIO_CHECK(AUDIO_GetEffectStatus(audio, &status) IS ERR::Okay and status.Truncated IS 1);
   auto application_effects = std::move(set.Effects);
   AUDIO_CHECK(AUDIO_GetEffectStatus(audio, &status) IS ERR::Okay and status.Truncated IS 1);
   set.Effects = std::move(application_effects);
   audio->GlobalEffects->Truncated = false;
   channel.State = CHS::PLAYING;
   AUDIO_CHECK(AUDIO_GetEffectStatus(audio, &status) IS ERR::Okay and status.State IS ADS::ACTIVE);
   channel.State = CHS::STOPPED;
   AUDIO_CHECK(AUDIO_GetEffectStatus(audio, &status) IS ERR::Okay and status.State IS ADS::IDLE);
   AUDIO_CHECK(SET_MaxDrain(audio, 1) IS ERR::InvalidState);


#ifdef ALSA_ENABLED
   // Drive the real worker algorithm with real mixer output and forced partial writes / EAGAIN.
   struct Backend {
      extAudio *Audio;
      std::array<float, 17> Buffer{};
      std::vector<float> Accepted;
      int Writes = 0, Waits = 0, DrainWaits = 0;
      bool Running = false, Stop = false;
      bool stopping() const { return Stop; }
      int rate() const { return 1000; }
      uint64_t period_frames() const { return 17; }
      uint64_t buffer_frames() const { return 51; }
      bool active(bool Apply) { return audio_playing(Audio); }
      bool produce(bool Started) {
         if (!audio_playing(Audio) and Started) return false;
         mix_data(Audio, 17, Buffer.data());
         return true;
      }
      int64_t available() const { return 51; }
      int64_t write(size_t Offset, size_t Frames) {
         if (++Writes IS 2) return -EAGAIN;
         const auto count = std::min(Frames, size_t(5));
         Accepted.insert(Accepted.end(), Buffer.begin() + Offset, Buffer.begin() + Offset + count);
         return count;
      }
      bool running() const { return Running; }
      int start() { Running = true; return 0; }
      int prepare() { Running = false; return 0; }
      int resume() { return 0; }
      int recover(int Error) { return Error; }
      void drop() { Running = false; }
      int64_t delay() { return !audio_playing(Audio) and !DrainWaits ? 23 : 0; }
      void fail(int Error) { Stop = true; }
      int wait(bool Device, int Timeout) {
         if (!Device and Timeout > 0) ++DrainWaits;
         if (Timeout < 0 or ++Waits > 100) Stop = true;
         return 0;
      }
   } backend{audio};
   backend.Accepted.reserve(1024);
   channel.State = CHS::PLAYING;
   channel.Position = channel.PositionLow = 0;
   set.Effects->reset();
   audio->GlobalEffects->reset();
   const auto stats = run_audio_worker(backend);
   AUDIO_CHECK(backend.Accepted.size() IS rendered.size());
   AUDIO_CHECK(backend.Accepted IS rendered);
   AUDIO_CHECK(backend.DrainWaits IS 1 and stats.DelayMaximum IS 23);

   // A stop with no pending DSP must still publish the final partial meter interval, without another mix call.
   application.processor = std::make_shared<audio_tests_audio_effect::TestProcessor>(1, 0);
   application.reset_meter(*set.Effects->Generation);
   std::array<float, 7> last_samples{};
   application.process(last_samples.data(), last_samples.size());
   AUDIO_CHECK(application.MeterFrames IS 7);
   AUDIO_CHECK(!audio_playing(audio));
   AUDIO_CHECK(application.Meter.Interval IS 7 and application.Meter.Flags IS (AMF::VALID|AMF::IDLE));
#endif
}
#endif

class PreparedDelay final : public AudioEffectProcessor {
   int channels = 1;
   size_t cursor = 0, occupied = 0;
   std::vector<float> storage;
   class Configuration final : public AudioEffectConfiguration {
      PreparedDelay *Owner;
      int Channels, Frames;
      std::vector<float> Storage;
   public:
      Configuration(PreparedDelay *Processor, int Rate, bool Stereo) : Owner(Processor),
         Channels(Stereo ? 2 : 1), Frames((Rate + 999) / 1000), Storage(Frames * Channels) { }
      int64_t latency() const override { return Frames; }
      void publish() override {
         Owner->storage.swap(Storage);
         Owner->channels = Channels;
         Owner->cursor = Owner->occupied = 0;
      }
   };
public:
   bool Reject = false;
   ERR prepare(int Rate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) override {
      if (Reject) return ERR::InvalidValue;
      Result = std::make_unique<Configuration>(this, Rate, Stereo);
      return ERR::Okay;
   }
   int64_t latency() const override { return storage.size() / channels; }
   AudioTail tail() const override { return AudioTail::FINITE; }
   uint64_t tail_frames() const override { return latency(); }
   bool pending() const override { return occupied != 0; }
   void reset() override { std::fill(storage.begin(), storage.end(), 0); cursor = occupied = 0; }
   void process(float *Buffer, int Frames) override {
      for (int i = 0; i < Frames * channels; ++i) {
         const float input = Buffer[i];
         Buffer[i] = storage[cursor];
         if (storage[cursor] != 0) --occupied;
         storage[cursor] = input;
         if (input != 0) ++occupied;
         cursor = (cursor + 1) % storage.size();
      }
   }
};

static void preparation(AudioTestContext &Test)
{
   auto processor = std::make_unique<PreparedDelay>();
   auto pointer = processor.get();
   Fixture fixture(44100, 1, std::move(processor));
   for (int rate : {44100, 48000, 96000}) for (bool stereo : {false, true}) {
      const auto generation = *fixture.Chain->Generation;
      AUDIO_REQUIRE(configure_effects(*fixture.Chain, rate, stereo) IS ERR::Okay);
      AUDIO_CHECK(*fixture.Chain->Generation > generation);
      AUDIO_CHECK(fixture.Effect.CommittedLatency IS (rate + 999) / 1000);
      std::vector<float> samples(200 * (stereo ? 2 : 1));
      samples[0] = 1;
      fixture.Effect.process(samples.data(), 200);
      for (int i = 0; i < 200; ++i) {
         AUDIO_CHECK(samples[i * (stereo ? 2 : 1)] IS (i IS fixture.Effect.CommittedLatency ? 1.0f : 0.0f));
      }
   }
   const auto generation = *fixture.Chain->Generation;
   pointer->Reject = true;
   AUDIO_CHECK(configure_effects(*fixture.Chain, 44100, false) IS ERR::InvalidValue);
   AUDIO_CHECK(*fixture.Chain->Generation IS generation and fixture.Effect.OutputRate IS 96000);
   AUDIO_CHECK(fixture.Effect.CommittedLatency IS 96);

   class Update final : public AudioParamUpdate {
   public:
      int64_t latency() const override { return 123; }
      void publish(extAudioEffect *Effect) override { Effect->CommittedLatency = 123; }
   };
   const AudioEffectSchema schema = {
      .ClassName = "LatencyFixture", .Version = 1,
      .Read = [](extAudioEffect *Effect, AudioParamState &State) {},
      .Prepare = [](extAudioEffect *Effect, const AudioParamState &State, int Rate) ->
         std::unique_ptr<AudioParamUpdate> { return std::make_unique<Update>(); }
   };
   fixture.Effect.Schema = &schema;
   AUDIO_CHECK(effect_commit(&fixture.Effect, {}) IS ERR::InvalidState);
   AUDIO_CHECK(*fixture.Chain->Generation IS generation and fixture.Effect.CommittedLatency IS 96);
}

static void render_costs()
{
   Fixture fixture(48000, 2, nullptr);
   const std::vector<AudioEQBand> bands = {
      {EQB::LOW_SHELF, 120, 3, 0.707}, {EQB::PEAK, 1800, -3, 1}, {EQB::HIGH_SHELF, 8000, 2, 0.707}
   };
   fixture.Effect.set_processor(std::make_unique<EqualiserProcessor>(&fixture.Effect, bands, 0));
   EqualiserProcessor bare(&fixture.Effect, bands, 0);
   bare.reset();
   AudioEffectChain empty(fixture.Mutex);
   empty.Rate = 48000;
   empty.Stereo = true;
   std::array<float, 512> buffer;
   auto measure = [&](auto Process) {
      double maximum = 0;
      const auto start = std::chrono::steady_clock::now();
      for (int i = 0; i < 2000; ++i) {
         std::fill(buffer.begin(), buffer.end(), 0.25f);
         const auto before = std::chrono::steady_clock::now();
         Process();
         maximum = std::max(maximum,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count());
      }
      return std::pair(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), maximum);
   };
   const auto plain = measure([&] { bare.process(buffer.data(), 256); });
   const auto metered = measure([&] { render_effects(*fixture.Chain, buffer.data(), 256, 256, 48000, true); });
   const auto none = measure([&] { render_effects(empty, buffer.data(), 256, 256, 48000, true); });
   kt::Log("DSP headroom").msg("512000 frames: EQ direct %.6f s, EQ with meters %.6f s, empty chain %.6f s.",
      plain.first, metered.first, none.first);
   kt::Log("DSP headroom").msg("Maximum 256-frame render: EQ %.3f us, empty chain %.3f us.",
      metered.second * 1e6, none.second * 1e6);
}

static void run(AudioTestContext &Test)
{
   render_costs();
   preparation(Test);
   delays(Test);
   deadlines(Test);
   upstream_silence(Test);
   meters(Test);
   equaliser_tail(Test);
   contention(Test);
#ifdef AUDIO_WORKER
   common_mixer(Test);
#endif
}

} // namespace audio_tests_dsp_infrastructure
