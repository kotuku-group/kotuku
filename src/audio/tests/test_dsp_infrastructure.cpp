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

static std::span<const int> test_layout(int Channels)
{
   return Channels IS 2 ? std::span<const int>(glLayoutStereo) : std::span<const int>(glLayoutMono);
}

struct Fixture {
   std::shared_ptr<std::recursive_mutex> Mutex = std::make_shared<std::recursive_mutex>();
   std::shared_ptr<AudioEffectChain> Chain = std::make_shared<AudioEffectChain>(Mutex);
   extAudioEffect Effect{nullptr, 0};
   Fixture(int Rate, int Channels, std::unique_ptr<AudioEffectProcessor> Processor,
      const AudioEffectSchema *Schema = nullptr) {
      Effect.Chain = Chain;
      Effect.Schema = Schema;
      Effect.OutputRate = Chain->Rate = Rate;
      Chain->Stereo = Channels IS 2;
      Chain->Layout.assign(test_layout(Channels).begin(), test_layout(Channels).end());
      Effect.set_layout(test_layout(Channels));
      Chain->Effects.push_back(&Effect);
      if (Processor) Effect.set_processor(std::move(Processor));
   }
};

// Scalar output templates for meter tests: per-channel peaks and one global value.

static const AudioOutputDesc glMeterOutputs[] = {
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", {}, "dBFS", "channel", "sample-peak",
      AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", {}, "dBFS", "channel", "sample-peak",
      AudioMeterSource::OUTPUT_PEAK },
   { "gain_reduction", AudioOutputKind::SCALAR, "Gain Reduction", {}, "dB", "global", "gain-reduction",
      AudioMeterSource::GAIN_REDUCTION }
};

static const AudioEffectSchema glMeterSchema = {
   .ClassName = "MeterFixture", .Version = 1, .Outputs = glMeterOutputs,
   .Read = [](extAudioEffect *, AudioParamState &) {}, .Apply = [](extAudioEffect *, const AudioParamState &) {}
};

struct Reading {
   std::vector<double> Values;
   std::vector<int> Flags;
   MeterReading Meta;
   ERR Error;
};

static Reading read(extAudioEffect &Effect)
{
   Reading result;
   auto chain = Effect.Chain.lock();
   std::unique_lock<std::recursive_mutex> lock;
   if (chain) lock = std::unique_lock(*chain->Mutex);
   Effect.read_meter(chain.get(), result.Meta);
   for (const auto &value : result.Meta.Values) {
      result.Values.push_back(value.Value);
      result.Flags.push_back(int(value.Flags));
   }
   result.Error = ERR::Okay;
   return result;
}

static void delays(AudioTestContext &Test)
{
   for (int rate : {44100, 48000, 96000}) for (int channels : {1, 2}) {
      Fixture fixture(rate, channels, std::make_unique<Delay>(107, channels));
      auto &chain = *fixture.Chain;
      extAudioEffect second(nullptr, 0);
      second.Chain = fixture.Chain;
      second.OutputRate = rate;
      second.set_layout(test_layout(channels));
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
   Fixture fixture(44100, 2, std::make_unique<Delay>(1, 2), &glMeterSchema);
   auto &effect = fixture.Effect;

   // Stereo publishes one peak per channel for input and output, then the global value.

   AUDIO_REQUIRE(effect.Meters.size() IS 5);
   AUDIO_CHECK(std::string_view(effect.Meters[0].Key) IS "input_peak_left");
   AUDIO_CHECK(std::string_view(effect.Meters[1].Key) IS "input_peak_right");
   AUDIO_CHECK(std::string_view(effect.Meters[2].Key) IS "output_peak_left");
   AUDIO_CHECK(std::string_view(effect.Meters[3].Label) IS "Output Peak Right");
   AUDIO_CHECK(effect.Meters[1].Channel IS int(SPK::FRONT_RIGHT) and effect.Meters[4].Channel IS 0);
   AUDIO_CHECK(std::string_view(effect.Meters[4].Key) IS "gain_reduction");

   std::vector<float> samples(2205 * 2, 0.5f);
   for (size_t i = 1; i < samples.size(); i += 2) samples[i] = 0.25f;
   effect.process(samples.data(), 1000);
   AUDIO_CHECK(effect.Meter.Sequence IS 0);
   effect.process(samples.data() + 2000, 1205);
   auto snapshot = read(effect);
   AUDIO_REQUIRE(snapshot.Error IS ERR::Okay);
   AUDIO_CHECK(snapshot.Meta.Sequence IS 1 and snapshot.Meta.Interval IS 2205 and snapshot.Meta.Position IS 2205);
   AUDIO_CHECK(std::abs(snapshot.Values[0] + 6.020599913) < 1e-6);
   AUDIO_CHECK(std::abs(snapshot.Values[1] + 12.041199827) < 1e-6);
   AUDIO_CHECK(snapshot.Meta.Flags IS AMF::VALID);
   for (auto flags : snapshot.Flags) AUDIO_CHECK(flags IS int(AMV::VALID));
   AUDIO_CHECK(read(effect).Meta.Sequence IS snapshot.Meta.Sequence);

   std::array<float, 20> silence{};
   effect.process(silence.data(), 10);
   effect.idle();
   AUDIO_CHECK(effect.Meter.Sequence IS 2 and effect.Meter.Interval IS 10 and
      effect.Meter.Flags IS (AMF::VALID|AMF::IDLE));
   snapshot = read(effect);
   AUDIO_CHECK(snapshot.Values[0] IS -120 and snapshot.Flags[0] IS int(AMV::VALID|AMV::FLOOR));
   AUDIO_CHECK(snapshot.Flags[1] IS int(AMV::VALID|AMV::FLOOR) and snapshot.Flags[4] IS int(AMV::VALID));

   // A reading reports the same generation as GetMeterLayout(), so a client with current descriptors never re-reads.

   AUDIO_CHECK(uint64_t(snapshot.Meta.ID) IS effect.meter_generation(fixture.Chain.get()));

   fixture.Chain->reset();
   snapshot = read(effect);
   AUDIO_CHECK((snapshot.Meta.Flags & AMF::NO_SAMPLES) != AMF::NIL and effect.Meter.Interval IS 0);
   for (auto value_flags : snapshot.Flags) AUDIO_CHECK(value_flags IS 0);
   effect.Flags = AEF::BYPASS;
   AUDIO_CHECK(read(effect).Meta.Flags IS AMF::BYPASSED);
   effect.Flags = AEF::NIL;

   // Reapplying the same layout keeps the descriptors in place; a different layout replaces them, and the reset that
   // accompanies every layout change gives readings a new generation.

   const auto stereo_generation = effect.meter_generation(fixture.Chain.get());
   const auto stereo_descriptors = effect.Meters.data();
   effect.set_layout(glLayoutStereo);
   AUDIO_CHECK(effect.Meters.data() IS stereo_descriptors);
   effect.set_layout(glLayoutMono);
   fixture.Chain->reset();
   AUDIO_CHECK(effect.Meters.size() IS 3);
   AUDIO_CHECK(std::string_view(effect.Meters[0].Key) IS "input_peak_centre");
   AUDIO_CHECK(effect.Meters[0].Channel IS int(SPK::CENTRE));
   snapshot = read(effect);
   AUDIO_CHECK(uint64_t(snapshot.Meta.ID) != stereo_generation and snapshot.Values.size() IS 3);
   AUDIO_CHECK(uint64_t(snapshot.Meta.ID) IS effect.meter_generation(fixture.Chain.get()));

   const auto xml = build_schema_xml(glMeterSchema, effect.Meters);
   AUDIO_CHECK(xml.find("key=\"input_peak_centre\" type=\"scalar\" label=\"Input Peak Centre\" unit=\"dBFS\" "
      "scope=\"channel\" channel=\"1\" semantics=\"sample-peak\" slot=\"0\"/>") != std::string::npos);
   AUDIO_CHECK(xml.find("key=\"gain_reduction\"") != std::string::npos);
   AUDIO_CHECK(xml.find("slot=\"2\"") != std::string::npos and xml.find("slot=\"3\"") IS std::string::npos);

   effect.detach();
   snapshot = read(effect);
   AUDIO_CHECK(snapshot.Meta.Flags IS AMF::DISCONNECTED and (snapshot.Meta.Flags & AMF::VALID) IS AMF::NIL);

   // More than 32 values remain addressable, with independent flags for each value.

   std::vector<int> wide;
   for (int c = 0; c < 20; c++) wide.push_back(int(SPK::DISCRETE) + c);
   Fixture synthetic(1000, 1, nullptr, &glMeterSchema);
   synthetic.Effect.set_layout(wide);
   AUDIO_REQUIRE(synthetic.Effect.Meters.size() IS 41);
   AUDIO_CHECK(std::string_view(synthetic.Effect.Meters[19].Key) IS "input_peak_discrete_19");
   for (int c = 0; c < 20; c++) {
      synthetic.Effect.InputPeaks[c] = (c % 2) ? 0.5 : 0;
      synthetic.Effect.OutputPeaks[c] = (c % 3) ? 0.25 : 0;
   }
   synthetic.Effect.MeterFrames = 1;
   synthetic.Effect.publish_meter(AMF::NIL);
   auto wide_reading = read(synthetic.Effect);
   AUDIO_REQUIRE(wide_reading.Error IS ERR::Okay and wide_reading.Values.size() IS 41);
   for (int c = 0; c < 20; c++) {
      AUDIO_CHECK(wide_reading.Flags[c] IS int((c % 2) ? AMV::VALID : (AMV::VALID|AMV::FLOOR)));
      AUDIO_CHECK(wide_reading.Flags[20 + c] IS int((c % 3) ? AMV::VALID : (AMV::VALID|AMV::FLOOR)));
   }
   AUDIO_CHECK(std::abs(wide_reading.Values[39] + 12.041199827) < 1e-6 and wide_reading.Flags[40] IS int(AMV::VALID));

   Fixture integer_output(1000, 1, std::make_unique<Delay>(1, 1), &glMeterSchema);
   integer_output.Chain->MeterScale = 32768; // Both integer output formats use 16-bit internal mixer units.
   std::array<float, 50> integer_samples;
   std::fill(integer_samples.begin(), integer_samples.end(), 16384);
   integer_output.Effect.process(integer_samples.data(), integer_samples.size());
   AUDIO_CHECK(std::abs(integer_output.Effect.Meter.Values[0] + 6.020599913) < 1e-6);

   Fixture reduction(1000, 1, std::make_unique<Feedback>(), &glMeterSchema);
   std::array<float, 50> input{};
   input[0] = 1;
   reduction.Effect.process(input.data(), input.size());
   AUDIO_CHECK(reduction.Effect.Meter.Values[2] IS 3);
}

static void meter_layout_lifetime(AudioTestContext &Test)
{
   auto release = [](MeterLayout *Layout) { if (Layout) FreeResource(Layout); };
   std::unique_ptr<MeterLayout, decltype(release)> snapshot(nullptr, release);
   {
      Fixture fixture(48000, 2, nullptr, &glMeterSchema);
      fx::GetMeterLayout args{};
      AUDIO_REQUIRE(AUDIOEFFECT_GetMeterLayout(&fixture.Effect, &args) IS ERR::Okay);
      snapshot.reset(args.Layout);
      AUDIO_REQUIRE(snapshot and snapshot->Meters.size() IS 5);
      const auto generation = snapshot->ID;
      AUDIO_CHECK(snapshot->Meters[1].Key IS "input_peak_right");

      fixture.Effect.set_layout(glLayoutMono);
      fixture.Chain->reset();
      AUDIO_CHECK(snapshot->ID IS generation and snapshot->Meters.size() IS 5);
      AUDIO_CHECK(snapshot->Meters[1].Key IS "input_peak_right");
      AUDIO_REQUIRE(AUDIOEFFECT_GetMeterLayout(&fixture.Effect, &args) IS ERR::Okay);
      std::unique_ptr<MeterLayout, decltype(release)> mono(args.Layout, release);
      AUDIO_CHECK(mono->ID != generation and mono->Meters.size() IS 3);
      AUDIO_CHECK(mono->Meters[0].Key IS "input_peak_centre");
   }
   // Every descriptor field, including string storage, survives the destruction of the effect and its chain.
   const auto &meter = snapshot->Meters[1];
   AUDIO_CHECK(meter.Key IS "input_peak_right" and meter.Label IS "Input Peak Right");
   AUDIO_CHECK(meter.Unit IS "dBFS" and meter.Scope IS "channel" and meter.Semantics IS "sample-peak");
   AUDIO_CHECK(meter.Slot IS 1 and meter.Channel IS int(SPK::FRONT_RIGHT));
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
   Fixture fixture(48000, 2, std::make_unique<Delay>(128, 2), &glMeterSchema);
   std::atomic<bool> done{false};
   std::atomic<uint64_t> reads{0};
   auto reader = std::thread([&] {
      uint64_t sequence = 0;
      while (!done) {
         auto snapshot = read(fixture.Effect);
         AUDIO_CHECK(uint64_t(snapshot.Meta.Sequence) >= sequence);
         sequence = snapshot.Meta.Sequence;
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
   sample.SampleType = PCM::S16_MONO;
   sample.FrameBytes = 2;
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
   set.Effects->Layout = audio->GlobalEffects->Layout = { int(SPK::CENTRE) };
   application.set_layout(glLayoutMono);
   global.set_layout(glLayoutMono);
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
      AUDIO_REQUIRE(configure_effects(*fixture.Chain, rate, test_layout(stereo ? 2 : 1)) IS ERR::Okay);
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
   AUDIO_CHECK(configure_effects(*fixture.Chain, 44100, glLayoutMono) IS ERR::InvalidValue);
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

// Device-independent failure injection covers the transaction shared by activation and Windows reopening.
static void output_transaction(AudioTestContext &Test)
{
   extAudio *audio;
   AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
   std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
   AUDIO_REQUIRE(InitObject(audio) IS ERR::Okay);
   audio->Sets.resize(2);
   auto &set = audio->Sets[1];
   set.Effects = std::make_shared<AudioEffectChain>(audio->MixerLock);
   set.Effects->Generation = audio->GlobalEffects->Generation;

   extAudioEffect global(nullptr, 0), application(nullptr, 0);
   global.Chain = audio->GlobalEffects;
   application.Chain = set.Effects;
   audio->GlobalEffects->Effects.push_back(&global);
   set.Effects->Effects.push_back(&application);
   auto first = std::make_shared<PreparedDelay>();
   auto second = std::make_shared<PreparedDelay>();
   global.processor = first;
   application.processor = second;
   audio->OutputRate = 48000;

   // A failure in the last chain must not publish the already prepared first chain, even on first activation.
   second->Reject = true;
   AUDIO_CHECK(commit_audio_output(audio, glLayoutStereo, nullptr) IS ERR::InvalidValue);
   AUDIO_CHECK(audio->CommittedLayout.empty() and audio->OutputGeneration IS 0);
   AUDIO_CHECK(!global.FormatCommitted and !application.FormatCommitted and first->latency() IS 0);
   second->Reject = false;
   AUDIO_REQUIRE(commit_audio_output(audio, glLayoutStereo, nullptr) IS ERR::Okay);
   const auto generation = audio->OutputGeneration;
   AUDIO_CHECK(global.FormatGeneration IS generation and application.FormatGeneration IS generation);
   AUDIO_CHECK(audio->CommittedRate IS 48000 and audio->OutputActive);
   AUDIO_REQUIRE(acDeactivate(audio) IS ERR::Okay);

   audio->OutputRate = 96000; // A candidate rate negotiated by a replacement device.
   second->Reject = true;
   AUDIO_CHECK(commit_audio_output(audio, glLayoutMono, nullptr) IS ERR::InvalidValue);
   second->Reject = false;
   AUDIO_CHECK(commit_audio_output(audio, glLayoutMono, [](extAudio *) { return ERR::CreateResource; }) IS
      ERR::CreateResource);
   AUDIO_CHECK(!audio->OutputActive and !audio->EffectConfigured);
   AUDIO_CHECK(audio->OutputGeneration IS generation and layout_is(audio->CommittedLayout, glLayoutStereo));
   AUDIO_CHECK(global.FormatGeneration IS generation and application.FormatGeneration IS generation);
   AUDIO_CHECK(global.OutputRate IS 48000 and application.OutputRate IS 48000);
   AUDIO_CHECK(first->latency() IS 48 and second->latency() IS 48);
   snd::GetOutputFormat output{};
   AUDIO_REQUIRE(AUDIO_GetOutputFormat(audio, &output) IS ERR::Okay);
   AUDIO_CHECK(output.SampleRate IS 48000 and output.State IS AFS::INACTIVE);
   AUDIO_CHECK(uint64_t(output.Generation) IS generation);

   AUDIO_REQUIRE(commit_audio_output(audio, glLayoutMono, nullptr) IS ERR::Okay);
   AUDIO_CHECK(audio->OutputGeneration IS generation + 1 and audio->CommittedRate IS 96000);
   AUDIO_CHECK(global.FormatGeneration IS audio->OutputGeneration and
      application.FormatGeneration IS audio->OutputGeneration);
   AUDIO_CHECK(global.Layout.size() IS 1 and application.Layout.size() IS 1);
   AUDIO_CHECK(first->latency() IS 96 and second->latency() IS 96);
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
   empty.Layout.assign(std::begin(glLayoutStereo), std::end(glLayoutStereo));
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

#ifdef AUDIO_WORKER
// Stop notifications on a path with effects wait for the tail, but not for audio that belongs to other voices.

static void drained_stops(AudioTestContext &Test)
{
   extAudio *audio;
   AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
   std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
   AUDIO_REQUIRE(InitObject(audio) IS ERR::Okay);
   audio->OutputRate = 1000;
   audio->EffectConfigured = true;
   audio->Samples.resize(2);
   audio->Sets.resize(3);
   for (int s = 1; s <= 2; s++) {
      audio->Sets[s].Channel.resize(2);
      for (int c = 0; c < 2; c++) {
         auto &voice = audio->Sets[s].Channel[c];
         voice.SampleHandle = 1;
         voice.Handle = (s << 16) | c;
         voice.State = CHS::STOPPED;
         voice.Frequency = 0;
      }
   }

   // Without effects the stop is due immediately after the device queue.
   auto &voice = audio->Sets[1].Channel[0];
   voice.State = CHS::PLAYING;
   voice.Frequency = 1000;
   audio->finish(voice, true);
   AUDIO_REQUIRE(audio->NotificationCount IS 1);
   AUDIO_CHECK((not audio->Notifications[0].AwaitDrain) and audio->Notifications[0].Due < INT64_MAX);
   audio->NotificationCount = 0;

   auto &set = audio->Sets[1];
   set.Effects = std::make_shared<AudioEffectChain>(audio->MixerLock);
   extAudioEffect effect(nullptr, 0);
   effect.Chain = set.Effects;
   effect.OutputRate = set.Effects->Rate = 1000;
   set.Effects->Layout = { int(SPK::CENTRE) };
   effect.set_layout(glLayoutMono);
   set.Effects->Effects.push_back(&effect);
   AUDIO_REQUIRE(effect.set_processor(std::make_unique<Delay>(107, 1)) IS ERR::Okay);

   auto queue = [&]() {
      voice.State = CHS::PLAYING;
      voice.Frequency = 1000;
      audio->finish(voice, true);
      return &audio->Notifications[audio->NotificationCount - 1];
   };

   auto excite = [&]() {
      float value = 1.0f;
      effect.process(&value, 1);
   };

   // A pending tail holds the stop until the chain drains.
   excite();
   auto event = queue();
   AUDIO_CHECK(event->AwaitDrain and event->Due IS INT64_MAX);
   const auto now = PreciseTime();
   resolve_drained_stops(audio, now);
   AUDIO_CHECK(event->AwaitDrain);
   for (int i = 0; i < 107; i++) {
      float silence = 0;
      effect.process(&silence, 1);
   }
   AUDIO_CHECK(not set.Effects->pending());
   resolve_drained_stops(audio, now);
   AUDIO_CHECK((not event->AwaitDrain) and event->Due >= now and event->Due < now + 1000000);
   audio->NotificationCount = 0;

   // Another voice on the set makes the tail inseparable, so the stop follows the path's estimated decay.  The
   // delay contributes only its latency.
   excite();
   event = queue();
   auto &other = set.Channel[1];
   other.State = CHS::PLAYING;
   other.Frequency = 1000;
   resolve_drained_stops(audio, now);
   AUDIO_CHECK((not event->AwaitDrain) and event->Due IS std::max(now, event->Audible + 107000));
   audio->NotificationCount = 0;

   class Ringing final : public AudioEffectProcessor {
   public:
      uint64_t Estimate = 500;
      bool pending() const override { return true; }
      uint64_t decay_estimate() const override { return Estimate; }
      void reset() override { }
      void process(float *, int) override { }
   };

   extAudioEffect ringing(nullptr, 0);
   ringing.Chain = set.Effects;
   ringing.OutputRate = 1000;
   ringing.set_layout(glLayoutMono);
   set.Effects->Effects.push_back(&ringing);
   auto ringing_processor = std::make_unique<Ringing>();
   auto ringer = ringing_processor.get();
   AUDIO_REQUIRE(ringing.set_processor(std::move(ringing_processor)) IS ERR::Okay);
   float input = 0;
   ringing.process(&input, 1);

   event = queue();
   resolve_drained_stops(audio, event->Audible);
   AUDIO_CHECK((not event->AwaitDrain) and event->Due IS event->Audible + 607000);

   // An estimate that has already elapsed is due immediately, and no estimate exceeds the drain deadline.
   event = queue();
   resolve_drained_stops(audio, event->Audible + 900000);
   AUDIO_CHECK(event->Due IS event->Audible + 900000);
   ringer->Estimate = UINT64_MAX / 2;
   event = queue();
   resolve_drained_stops(audio, event->Audible);
   AUDIO_CHECK(event->Due IS event->Deadline);

   // Bypassed effects do not contribute.
   ringing.Flags = AEF::BYPASS;
   AUDIO_CHECK(set.Effects->decay_estimate() IS 107);
   ringing.Flags = AEF::NIL;
   std::erase(set.Effects->Effects, &ringing);
   ringing.Chain.reset();
   other.State = CHS::STOPPED;
   other.Frequency = 0;
   audio->NotificationCount = 0;

   // The deadline and deactivation both end the wait while the tail is still pending.
   event = queue();
   AUDIO_CHECK(set.Effects->pending());
   resolve_drained_stops(audio, event->Deadline);
   AUDIO_CHECK(not event->AwaitDrain);
   event = queue();
   audio->EffectConfigured = false;
   resolve_drained_stops(audio, now);
   AUDIO_CHECK((not event->AwaitDrain) and event->Due IS now);
   audio->EffectConfigured = true;
   audio->NotificationCount = 0;

   // A global tail is awaited only while no other set feeds the global chain.
   set.Effects->Effects.clear();
   effect.Chain = audio->GlobalEffects;
   audio->GlobalEffects->Layout = { int(SPK::CENTRE) };
   audio->GlobalEffects->Effects.push_back(&effect);
   event = queue();
   AUDIO_CHECK(event->AwaitDrain and audio->GlobalEffects->pending());
   resolve_drained_stops(audio, now);
   AUDIO_CHECK(event->AwaitDrain);
   auto &elsewhere = audio->Sets[2].Channel[0];
   elsewhere.State = CHS::PLAYING;
   elsewhere.Frequency = 1000;
   resolve_drained_stops(audio, now);
   AUDIO_CHECK(not event->AwaitDrain);
   elsewhere.State = CHS::STOPPED;
   elsewhere.Frequency = 0;
   audio->GlobalEffects->Effects.clear();
   effect.Chain.reset();
   audio->NotificationCount = 0;
}
#endif

//********************************************************************************************************************

static void run(AudioTestContext &Test)
{
   render_costs();
   preparation(Test);
   output_transaction(Test);
   delays(Test);
   deadlines(Test);
   upstream_silence(Test);
   meters(Test);
   meter_layout_lifetime(Test);
   equaliser_tail(Test);
   contention(Test);
#ifdef AUDIO_WORKER
   common_mixer(Test);
   drained_stops(Test);
#endif
}

} // namespace audio_tests_dsp_infrastructure
