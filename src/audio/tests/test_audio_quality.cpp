// Quality contract tests use the real field setters, configuration parser and device-free mixer.
namespace audio_tests_audio_quality {

static void boundaries(AudioTestContext &Test)
{
   extAudio *audio;
   AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
   std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
   AUDIO_REQUIRE(SET_Quality(audio, 80) IS ERR::Okay);
   for (int quality : { -1, 0, 9, 10, 32, 33, 65, 66, 100, 101 }) {
      const int previous = audio->Quality;
      const auto previous_flags = audio->Flags;
      const auto previous_mode = audio->RequestedQuality.Mode;
      const auto previous_resampling = audio->MixConfig.resampler;
      const auto error = audio->setQuality(quality);
      if (quality < 0 or quality > 100) {
         AUDIO_CHECK(error IS ERR::OutOfRange);
         AUDIO_CHECK(audio->Quality IS previous and audio->Flags IS previous_flags);
         AUDIO_CHECK(audio->RequestedQuality.Mode IS previous_mode);
         AUDIO_CHECK(audio->MixConfig.resampler IS previous_resampling);
         continue;
      }
      AUDIO_CHECK(error IS ERR::Okay and audio->Quality IS quality);
      AUDIO_CHECK(audio->Flags IS previous_flags);
      for (int flags : { 0, int(ADF::VOL_RAMPING), int(ADF::AUTO_SAVE | ADF::VOL_RAMPING) }) {
         AUDIO_CHECK(audio->setFlags(ADF(flags)) IS ERR::Okay);
         AUDIO_CHECK(audio->Flags IS ADF(flags));
         std::lock_guard lock(audio->MixerMutex);
         audio->snapshot_quality();
         AUDIO_CHECK(audio->EffectiveQuality.Mode IS audio_effective_quality_profile(quality)->Mode);
         AUDIO_CHECK(audio->WindowFlags IS audio->Flags);
         AUDIO_CHECK(audio->WindowConfig.resampler IS audio_quality_profile(quality)->Resampler);
      }
   }
   // Flags first and Quality first must converge on the same profile, with unrelated flags preserved.
   AUDIO_CHECK(audio->setFlags(ADF::VOL_RAMPING) IS ERR::Okay);
   AUDIO_CHECK(audio->setQuality(10) IS ERR::Okay);
   AUDIO_CHECK(audio->Flags IS ADF::VOL_RAMPING);
   AUDIO_CHECK(audio->setFlags(ADF::NIL) IS ERR::Okay);
   AUDIO_CHECK(audio->Flags IS ADF::NIL);
   for (int flags : { 1, 2, 4, 7, 64, -1 }) {
      const auto previous = audio->Flags;
      AUDIO_CHECK(audio->setFlags(ADF(flags)) IS ERR::Args);
      AUDIO_CHECK(audio->Flags IS previous);
   }
   SET_Flags(audio, 0); // Avoid AUTO_SAVE affecting the user's configuration on destruction.
   AUDIO_REQUIRE(InitObject(audio) IS ERR::Okay);
   AUDIO_CHECK(audio->setFlags(ADF::VOL_RAMPING) IS ERR::ImmutableField);
   AUDIO_CHECK(audio->setQuality(100) IS ERR::Okay); // Quality remains writable after initialisation.
}

static void configuration(AudioTestContext &Test)
{
   objConfig::create config = {};
   AUDIO_REQUIRE(config.ok());
   extAudio *audio;
   AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
   std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
   for (int quality = 0; quality <= 100; ++quality) {
      AUDIO_REQUIRE(audio->setQuality(quality) IS ERR::Okay);
      objFile::create sink = { fl::Size(4096), fl::Flags(FL::BUFFER | FL::READ | FL::WRITE) };
      AUDIO_REQUIRE(sink.ok());
      // Exercise the real persistence action, parse its serialised text, then use the constructor's loading path.
      SET_Flags(audio, 0); // Device-free persistence, without querying the system mixer.
      AUDIO_REQUIRE(audio->saveToObject(*sink) IS ERR::Okay);
      std::span<int8_t> text;
      AUDIO_REQUIRE(sink->getBuffer(text) IS ERR::Okay);
      AUDIO_REQUIRE(config->clear() IS ERR::Okay);
      AUDIO_REQUIRE(config->dataFeed(audio, DATA::TEXT, text.first(size_t(sink->Position))) IS ERR::Okay);
      AUDIO_REQUIRE(audio->setQuality(80) IS ERR::Okay);
      load_audio_quality(audio, *config);
      AUDIO_CHECK(audio->Quality IS quality);
      AUDIO_CHECK(audio->Flags IS ADF::NIL);
      AUDIO_CHECK(audio->MixConfig.resampler IS audio_quality_profile(quality)->Resampler);
   }
   for (const auto saved : { "-1", "101", "4294967376", "9999999999999999999999", "", "invalid", "80junk" }) {
      AUDIO_REQUIRE(audio->setQuality(80) IS ERR::Okay);
      AUDIO_REQUIRE(config->write("AUDIO", "Quality", saved) IS ERR::Okay);
      load_audio_quality(audio, *config);
      AUDIO_CHECK(audio->Quality IS 80 and audio->Flags IS ADF::NIL);
   }
   for (const auto saved : { "0x20", "010", "+66" }) {
      AUDIO_REQUIRE(config->write("AUDIO", "Quality", saved) IS ERR::Okay);
      load_audio_quality(audio, *config);
      AUDIO_CHECK(audio->Quality IS std::strtol(saved, nullptr, 0));
   }
   AUDIO_REQUIRE(audio->setQuality(80) IS ERR::Okay);
   AUDIO_REQUIRE(config->deleteKey("AUDIO", "Quality") IS ERR::Okay);
   load_audio_quality(audio, *config);
   AUDIO_CHECK(audio->Quality IS 80);
}

#ifdef AUDIO_WORKER
// Removing the derived interpolation flag must preserve Amiga restart behaviour at each source mode.
static void amiga_restart(AudioTestContext &Test)
{
   extAudio *audio;
   AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
   std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
   AUDIO_REQUIRE(SET_Flags(audio, 0) IS ERR::Okay);
   AUDIO_REQUIRE(InitObject(audio) IS ERR::Okay);
   audio->Samples.resize(2);
   auto &sample = audio->Samples[1];
   sample.SampleType = PCM::U8_MONO;
   sample.FrameBytes = 1;
   sample.SampleLength = SAMPLE(8);
   sample.Data.assign(8, 128);
   sample.LoopMode = LOOP::AMIGA;
   sample.Loop1Type = LTYPE::UNIDIRECTIONAL;
   sample.Loop1Start = SAMPLE(2);
   sample.Loop1End = SAMPLE(8);
   audio->Sets.resize(2);
   audio->Sets[1].Channel.resize(1);
   audio->Sets[1].Shadow.resize(1);
   struct WorkerScope {
      bool Previous = glAudioWorker;
      WorkerScope() { glAudioWorker = true; }
      ~WorkerScope() { glAudioWorker = Previous; }
   } worker_scope;
   for (int quality : { 0, 9, 10, 33, 66, 100 }) {
      AUDIO_REQUIRE(SET_Quality(audio, quality) IS ERR::Okay);
      auto &channel = audio->Sets[1].Channel[0];
      channel = AudioChannel{};
      channel.Handle = 1 << 16;
      channel.Frequency = 44100;
      channel.State = CHS::FINISHED;
      AUDIO_REQUIRE(snd::MixSample(audio, channel.Handle, 1) IS ERR::Okay);
      AUDIO_CHECK(channel.State IS (quality < 10 ? CHS::PLAYING : CHS::FINISHED));
      if (quality < 10) AUDIO_CHECK(channel.Position IS 2);
   }
}

// Compare partitioned renders to the selected source kernel for every valid value.
static void profile_output(AudioTestContext &Test)
{
   const int16_t source[] = { 8192, -4096, 16384, 0, -8192, 24576, 4096, -16384, 0 };
   constexpr int frames = 6;
   for (bool stereo : { false, true }) for (bool looped : { false, true })
   for (int frequency : { 500, 1000 }) for (int quality = 0; quality <= 100; ++quality) {
      extAudio *audio;
      AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
      std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
      AUDIO_REQUIRE(SET_Quality(audio, quality) IS ERR::Okay);
      AUDIO_REQUIRE(SET_Flags(audio, 0) IS ERR::Okay);
      AUDIO_REQUIRE(InitObject(audio) IS ERR::Okay);
      audio->OutputRate = 1000;
      audio->BitDepth = 32;
      audio->Stereo = stereo;
      audio->MasterVolume = 1;
      audio->Mute = false;
      audio->DriverBitSize = stereo ? 8 : 4;
      audio->MixElements = SAMPLE(5); // Exercise multiple mixer windows.
      audio->MixBuffer.resize(stereo ? 10 : 5);
      audio->Samples.resize(2);
      auto &sample = audio->Samples[1];
      sample.SampleType = PCM::S16_MONO;
      sample.FrameBytes = 2;
      sample.SampleLength = SAMPLE(std::size(source));
      if (looped) {
         sample.LoopMode = LOOP::SINGLE;
         sample.Loop1Type = LTYPE::UNIDIRECTIONAL;
         sample.Loop1Start = SAMPLE(0);
         sample.Loop1End = sample.SampleLength;
      }
      sample.Data.resize(sizeof(source));
      std::memcpy(sample.Data.data(), source, sizeof(source));
      audio->Sets.resize(2);
      audio->Sets[1].Channel.resize(1);
      auto &channel = audio->Sets[1].Channel[0];
      channel.SampleHandle = 1;
      channel.Handle = 1 << 16;
      channel.Frequency = frequency;
      channel.State = CHS::PLAYING;
      channel.LVolume = 0.75;
      channel.RVolume = 0.5;
      channel.PositionLow = frequency IS 1000 ? 0 : 16384;

      std::array<float, frames * 2> expected{}, actual{};
      auto dest = expected.data();
      set_mix_step(frequency IS 1000 ? 65536 : 32768);
      const AudioConfig config(stereo, audio_quality_profile(quality)->Resampler);
      // The ordinary path now interpolates actual neighbours before the source end.
      const MixingParams params = { (APTR)source, channel.PositionLow, frames, 0.75f, 0.5f, &dest,
         0, int64_t(std::size(source)), 0, int64_t(std::size(source)),
         looped ? LTYPE::UNIDIRECTIONAL : LTYPE::NIL };
      AudioMixer::dispatch_mix(config, PCM::S16_MONO, params);
      AUDIO_REQUIRE(mix_data(audio, frames, actual.data()) IS ERR::Okay);
      AUDIO_CHECK(std::memcmp(actual.data(), expected.data(), frames * (stereo ? 2 : 1) * sizeof(float)) IS 0);
      AUDIO_CHECK(audio->EffectiveQuality.Mode IS audio_effective_quality_profile(quality)->Mode);
      AUDIO_CHECK(audio->WindowConfig.resampler IS audio_quality_profile(quality)->Resampler);
      AUDIO_CHECK(channel.Position IS (frequency IS 1000 ? 6 : 3));
      AUDIO_CHECK(channel.PositionLow IS (frequency IS 1000 ? 0 : 16384));
   }
}

// A processor writes Quality between source mixing and conversion: that write must affect only the next window.
class LiveQualityWrite final : public AudioEffectProcessor {
   extAudio *audio;
public:
   explicit LiveQualityWrite(extAudio *Audio) : audio(Audio) { }
   void reset() override { }
   AudioTail tail() const override { return AudioTail::INDEFINITE; }
   bool pending() const override { return true; }
   void process(float *Buffer, int Frames) override {
      SET_Quality(audio, 100);
      std::fill_n(Buffer, Frames, 0.25f);
   }
};

static void live_write(AudioTestContext &Test)
{
   extAudio *audio;
   AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
   std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
   SET_Quality(audio, 0);
   SET_Flags(audio, 0);
   AUDIO_REQUIRE(InitObject(audio) IS ERR::Okay);
   audio->OutputRate = 1000;
   audio->BitDepth = 32;
   audio->Stereo = false;
   audio->DriverBitSize = 4;
   audio->MixElements = SAMPLE(3);
   audio->MixBuffer.resize(3);
   extAudioEffect effect(nullptr, 0);
   effect.Chain = audio->GlobalEffects;
   effect.OutputRate = audio->GlobalEffects->Rate = 1000;
   audio->GlobalEffects->Layout = { int(SPK::CENTRE) };
   effect.set_layout(glLayoutMono);
   audio->GlobalEffects->Effects.push_back(&effect);
   AUDIO_REQUIRE(effect.set_processor(std::make_unique<LiveQualityWrite>(audio)) IS ERR::Okay);
   effect.idle(); // Complete the initial reset so the synthetic source can report pending output.
   std::array<float, 6> output{};
   const std::array<float, 6> expected = { 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f };
   std::lock_guard lock(audio->MixerMutex);
   AUDIO_REQUIRE(mix_data(audio, output.size(), output.data()) IS ERR::Okay);
   AUDIO_CHECK(output IS expected);
   AUDIO_CHECK(audio->Quality IS 100 and audio->WindowConfig.resampler IS ResamplerMode::SINC);
   AUDIO_CHECK(audio->EffectiveQuality.Mode IS AudioQualityMode::HIGH);
   audio->GlobalEffects->Effects.clear();
}

#endif

static void window_boundary(AudioTestContext &Test)
{
   extAudio *audio;
   AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
   std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
   SET_Quality(audio, 0);
   {
      std::lock_guard lock(audio->MixerMutex);
      audio->snapshot_quality();
      // A recursive write (e.g. from a callback) cannot alter the current window's processing state.
      AUDIO_CHECK(audio->setQuality(100) IS ERR::Okay);
      AUDIO_CHECK(audio->RequestedQuality.Mode IS AudioQualityMode::HIGH);
      AUDIO_CHECK(audio->EffectiveQuality.Mode IS AudioQualityMode::DRAFT);
      AUDIO_CHECK(audio->WindowConfig.resampler IS ResamplerMode::NEAREST);
   }
   std::atomic<bool> started{false};
   std::future<ERR> write;
   {
      std::lock_guard lock(audio->MixerMutex);
      audio->snapshot_quality();
      write = std::async(std::launch::async, [&] {
         started.store(true, std::memory_order_release);
         return SET_Quality(audio, 10);
      });
      while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
      AUDIO_CHECK(write.wait_for(std::chrono::milliseconds(10)) IS std::future_status::timeout);
      AUDIO_CHECK(audio->EffectiveQuality.Mode IS AudioQualityMode::HIGH and
         audio->WindowConfig.resampler IS ResamplerMode::SINC);
   }
   AUDIO_CHECK(write.get() IS ERR::Okay);
   std::lock_guard lock(audio->MixerMutex);
   audio->snapshot_quality();
   AUDIO_CHECK(audio->EffectiveQuality.Mode IS AudioQualityMode::LOW and
      audio->WindowConfig.resampler IS ResamplerMode::LINEAR);
}

static void run(AudioTestContext &Test)
{
   boundaries(Test);
   configuration(Test);
#ifdef AUDIO_WORKER
   amiga_restart(Test);
   profile_output(Test);
   live_write(Test);
#endif
   window_boundary(Test);
}

} // namespace audio_tests_audio_quality
