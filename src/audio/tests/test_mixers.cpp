// Included by audio.cpp to exercise the module implementation.

namespace audio_tests_mixers {

template<typename T, bool StereoSource, bool StereoOutput, ResamplerMode Mode, size_t Size>
static int mix_fixture(T (&Source)[Size], int Position, int Frames, int Offset,
   float Left, float Right, float **Dest)
{
   constexpr auto format = std::is_same_v<T, uint8_t> ?
      (StereoSource ? PCM::U8_STEREO : PCM::U8_MONO) : (StereoSource ? PCM::S16_STEREO : PCM::S16_MONO);
   MixingParams params{Source, Position, Frames, Left, Right, Dest, 0, Size / (StereoSource ? 2 : 1)};
   return AudioMixer::dispatch_mix(AudioConfig(StereoOutput, Mode), format, params);
}


static void unsigned_mono(AudioTestContext &Test)
{
   uint8_t source[] = { 0, 128, 255 };
   float output[3] = {};
   auto dest = output;
   mix_fixture<uint8_t, false, false, ResamplerMode::NEAREST>(source, 0, 3, 1, 1, 1, &dest);
   AUDIO_CHECK(output[0] IS -1.0f and output[1] IS 0 and output[2] IS 127.0f / 128.0f);
}

static void mono_to_stereo(AudioTestContext &Test)
{
   int16_t source[] = { -1000, 0, 1000 };
   float output[6] = {};
   auto dest = output;
   mix_fixture<int16_t, false, true, ResamplerMode::NEAREST>(source, 0, 3, 1, 0.5f, 0.8f, &dest);
   for (int i = 0; i < 3; ++i) {
      AUDIO_CHECK(output[i * 2] IS source[i] * (0.5f / 32768.0f));
      AUDIO_CHECK(output[i * 2 + 1] IS source[i] * (0.8f / 32768.0f));
   }
}

static void unsigned_stereo(AudioTestContext &Test)
{
   uint8_t source[] = { 0, 255, 128, 0 };
   float output[4] = {};
   auto dest = output;
   mix_fixture<uint8_t, true, true, ResamplerMode::NEAREST>(source, 0, 2, 1, 1, 1, &dest);
   AUDIO_CHECK(output[0] IS -1.0f and output[1] IS 127.0f / 128.0f);
   AUDIO_CHECK(output[2] IS 0 and output[3] IS -1.0f);
}

static void volume_accumulation_and_position(AudioTestContext &Test)
{
   // One pass per gain covers scaling, additive mixing and cursor advancement on the same mixer path.
   int16_t source[] = { -1000, 0, 1000 };
   for (float volume : { 0.0f, 0.25f, 0.5f, 1.0f, 2.0f }) {
      float output[] = { 100, 100, 100 };
      auto dest = output;
      const int position = mix_fixture<int16_t, false, false, ResamplerMode::NEAREST>(
         source, 0, 3, 1, volume, volume, &dest);
      for (int i = 0; i < 3; ++i) AUDIO_CHECK(output[i] IS 100 + volume * source[i] / 32768.0f);
      AUDIO_CHECK(position IS 3 * 65536 and dest IS output + 3);
   }
}

static void interpolation(AudioTestContext &Test)
{
   int16_t source[] = { 0, 32767, 0, -32767 };
   float output[4] = {};
   auto dest = output;
   set_mix_step(32768);
   const int position = mix_fixture<int16_t, false, false, ResamplerMode::LINEAR>(source, 32768, 4, 1, 1, 1, &dest);
   AUDIO_CHECK(output[0] IS 16383.5f / 32768.0f and output[1] IS 32767.0f / 32768.0f);
   AUDIO_CHECK(output[2] IS 16383.5f / 32768.0f and output[3] IS 0);
   AUDIO_CHECK(position IS 5 * 32768 and dest IS output + 4);
   set_mix_step(65536);
}

static void pcm_endpoints_and_output_conversion(AudioTestContext &Test)
{
   uint8_t source8[] = { 0, 128, 255 };
   const float expected8[] = { -1.0f, 0, 127.0f / 128.0f };
   float mixed8[std::size(source8)]{};
   auto dest8 = mixed8;
   mix_fixture<uint8_t, false, false, ResamplerMode::NEAREST>(source8, 0, std::size(source8), 1, 1, 1, &dest8);
   AUDIO_CHECK(std::equal(std::begin(mixed8), std::end(mixed8), std::begin(expected8)));
   uint8_t output8[std::size(source8)]{};
   convert_samples<uint8_t>(mixed8, std::size(mixed8), output8);
   AUDIO_CHECK(std::equal(std::begin(output8), std::end(output8), std::begin(source8)));

   int16_t source16[] = { -32768, -32767, -1, 0, 1, 32767 };
   float mixed16[std::size(source16)]{};
   auto dest16 = mixed16;
   mix_fixture<int16_t, false, false, ResamplerMode::NEAREST>(source16, 0, std::size(source16), 1, 1, 1, &dest16);
   for (size_t i = 0; i < std::size(source16); ++i) {
      AUDIO_CHECK(mixed16[i] IS float(source16[i]) / 32768.0f);
   }
   int16_t output16[std::size(source16)]{};
   convert_samples<int16_t>(mixed16, std::size(mixed16), output16);
   AUDIO_CHECK(std::equal(std::begin(output16), std::end(output16), std::begin(source16)));

   float clipping[] = { -2, -1, -0.5f, 0, 0.5f, 1, 2 };
   const uint8_t clipped8[] = { 0, 0, 64, 128, 192, 255, 255 };
   const int16_t clipped16[] = { -32768, -32768, -16384, 0, 16384, 32767, 32767 };
   uint8_t converted8[std::size(clipping)]{};
   int16_t converted16[std::size(clipping)]{};
   float converted_float[std::size(clipping)]{};
   convert_samples<uint8_t>(clipping, std::size(clipping), converted8);
   convert_samples<int16_t>(clipping, std::size(clipping), converted16);
   convert_samples<float>(clipping, std::size(clipping), converted_float);
   AUDIO_CHECK(std::equal(std::begin(converted8), std::end(converted8), std::begin(clipped8)));
   AUDIO_CHECK(std::equal(std::begin(converted16), std::end(converted16), std::begin(clipped16)));
   const float clipped_float[] = { -1, -1, -0.5f, 0, 0.5f, 1, 1 };
   AUDIO_CHECK(std::equal(std::begin(converted_float), std::end(converted_float), std::begin(clipped_float)));
}



static void channel_gain_contract(AudioTestContext &Test)
{
   // The documented linear balance law, including clamped finite boundaries and mono output ignoring pan.
   struct Case { double Volume, Pan; bool Stereo; double Left, Right; };
   const Case cases[] = {
      { 1.0, 0, true, 1.0, 1.0 }, { 0.5, 0, true, 0.5, 0.5 }, { 0.8, -0.25, true, 0.8, 0.6 },
      { 0.8, 0.25, true, 0.6, 0.8 }, { 1.0, -1.0, true, 1.0, 0 }, { 1.0, 1.0, true, 0, 1.0 },
      { 2.0, -3.0, true, 1.0, 0 }, { -0.5, 0.5, true, 0, 0 }, { 0.5, 1.0, false, 0.5, 0.5 },
      { 1.0, -0.0, true, 1.0, 1.0 }, { 1e308, 1e308, true, 0, 1.0 }
   };
   for (const auto &c : cases) {
      auto gains = channel_gains(c.Volume, c.Pan, c.Stereo);
      AUDIO_CHECK(std::abs(gains.Left - c.Left) < 1e-12 and std::abs(gains.Right - c.Right) < 1e-12);
   }
}

// Device-free fixtures inspect the source bus before global effects and output conversion.
static void ramp_envelopes(AudioTestContext &Test)
{
   for (int quality : { 0, 80 }) for (bool enabled : { false, true })
   for (bool stereo : { false, true }) for (PCM type : { PCM::U8_MONO, PCM::U8_STEREO,
      PCM::S16_MONO, PCM::S16_STEREO }) for (int rate : { 44100, 48000, 96000 })
   for (int ratio : { 1, 2, 4 }) for (bool backward : { false, true }) {
      extAudio *audio;
      AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
      std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
      AUDIO_REQUIRE(SET_Quality(audio, quality) IS ERR::Okay);
      AUDIO_REQUIRE(SET_Flags(audio, enabled ? int(ADF::VOL_RAMPING) : 0) IS ERR::Okay);
      AUDIO_REQUIRE(InitObject(audio) IS ERR::Okay);
      audio->OutputRate = rate;
      audio->Stereo = stereo;
      audio->MasterVolume = 1;
      audio->Mute = false;
      audio->snapshot_quality();
      audio->Samples.resize(2);
      auto &sample = audio->Samples[1];
      sample.SampleType = type;
      const bool source_stereo = pcm_stereo(type);
      const bool source16 = type IS PCM::S16_MONO or type IS PCM::S16_STEREO;
      sample.FrameBytes = (source_stereo ? 2 : 1) * (source16 ? 2 : 1);
      sample.SampleLength = SAMPLE(4096);
      sample.Data.resize(4096 * sample.FrameBytes);
      if (source16) {
         for (size_t i = 0; i < sample.Data.size(); i += 2) {
            const int16_t value = 16384;
            std::memcpy(sample.Data.data() + i, &value, 2);
         }
      }
      else std::fill(sample.Data.begin(), sample.Data.end(), uint8_t(192));
      sample.LoopMode = LOOP::SINGLE;
      sample.Loop1Type = LTYPE::UNIDIRECTIONAL;
      sample.Loop1End = sample.SampleLength;
      AudioChannel initial{};
      initial.SampleHandle = 1;
      initial.Handle = 1 << 16;
      initial.Frequency = rate * ratio / 2;
      initial.Position = backward ? 3000 : 0;
      initial.State = CHS::PLAYING;
      initial.Volume = 1;
      if (backward) initial.Flags |= CHF::BACKWARD;
      AUDIO_REQUIRE(set_channel_volume(audio, &initial) IS ERR::Okay);
      AUDIO_CHECK(((initial.Flags & CHF::VOL_RAMP) != CHF::NIL) IS enabled);
      constexpr double scale = 0.5; // Equal stereo sides sum to unity before the mono half-gain.
      std::array<float, 240> reference{};
      int64_t final_position = 0;
      int final_fraction = 0;
      for (int block : { 120, 1, 7, 31 }) {
         auto channel = initial;
         std::array<float, 240> output{};
         for (int frame = 0; frame < 120;) {
            const int count = std::min(block, 120 - frame);
            mix_channel(audio, channel, count, output.data() + frame * (stereo ? 2 : 1));
            frame += count;
         }
         if (block IS 120) {
            reference = output;
            final_position = channel.Position;
            final_fraction = channel.PositionLow;
         }
         else {
            AUDIO_CHECK(std::equal(output.begin(), output.end(), reference.begin()));
            AUDIO_CHECK(channel.Position IS final_position and channel.PositionLow IS final_fraction);
         }
         AUDIO_CHECK(channel.LVolume IS 1 and channel.RVolume IS 1);
         AUDIO_CHECK((channel.Flags & CHF::VOL_RAMP) IS CHF::NIL);
         for (int i = 0; i < 120; ++i) {
            const double gain = enabled ? std::min(i * 0.01, 1.0) : 1.0;
            AUDIO_CHECK(std::abs(output[i * (stereo ? 2 : 1)] - scale * gain) < 1e-7);
            if (stereo) AUDIO_CHECK(output[i * 2 + 1] IS output[i * 2]);
         }
      }
      // Cross forward loop ends and exercise reverse routing without changing the constant source amplitude.
      for (LTYPE loop : { LTYPE::UNIDIRECTIONAL, LTYPE::BIDIRECTIONAL }) {
         sample.Loop1Type = loop;
         auto start = initial;
         start.Position = backward ? 3000 : 4080;
         std::array<float, 240> loop_reference{};
         int64_t loop_position = 0;
         int loop_fraction = 0;
         for (int block : { 120, 1, 7 }) {
            auto channel = start;
            std::array<float, 240> output{};
            for (int frame = 0; frame < 120;) {
               const int count = std::min(block, 120 - frame);
               mix_channel(audio, channel, count, output.data() + frame * (stereo ? 2 : 1));
               frame += count;
            }
            if (block IS 120) {
               loop_reference = output;
               loop_position = channel.Position;
               loop_fraction = channel.PositionLow;
            }
            else {
               AUDIO_CHECK(std::equal(output.begin(), output.end(), loop_reference.begin()));
               AUDIO_CHECK(channel.Position IS loop_position and channel.PositionLow IS loop_fraction);
            }
            for (int frame = 0; frame < 120; ++frame) {
               const double gain = enabled ? std::min(frame * 0.01, 1.0) : 1.0;
               AUDIO_CHECK(std::abs(output[frame * (stereo ? 2 : 1)] - scale * gain) < 1e-7);
            }
         }
      }
#ifdef AUDIO_WORKER
      // Ring consumption and starvation must not change the gain-envelope clock.
      if (!backward) {
         sample.Stream = true;
         sample.Prefilled = true;
         sample.EndOfSource = true;
         for (int block : { 120, 1, 7 }) {
            auto channel = initial;
            sample.Ring.Read = 0;
            sample.Ring.Used = sample.Data.size();
            std::array<float, 240> output{};
            for (int frame = 0; frame < 120;) {
               const int count = std::min(block, 120 - frame);
               mix_channel(audio, channel, count, output.data() + frame * (stereo ? 2 : 1));
               frame += count;
            }
            AUDIO_CHECK(std::equal(output.begin(), output.end(), reference.begin()));
            AUDIO_CHECK(channel.PositionLow IS final_fraction);
            AUDIO_CHECK(sample.Ring.Read IS size_t(final_position) * sample.FrameBytes);
         }
      }
#endif
   }
}

static void rapid_ramp_commands(AudioTestContext &Test)
{
   for (int quality : { 0, 80 }) for (bool stream : { false, true }) {
      extAudio *audio;
      AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
      std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
      AUDIO_REQUIRE(SET_Quality(audio, quality) IS ERR::Okay);
      AUDIO_REQUIRE(InitObject(audio) IS ERR::Okay);
      audio->OutputRate = 48000;
      audio->Stereo = true;
      audio->MasterVolume = 1;
      audio->Mute = false;
      audio->snapshot_quality();
      audio->Samples.resize(2);
      auto &sample = audio->Samples[1];
      sample.SampleType = PCM::U8_MONO;
      sample.FrameBytes = 1;
      sample.SampleLength = SAMPLE(4096);
      sample.Data.assign(4096, 192);
      sample.Stream = stream;
      sample.StreamLengthKnown = true;
      sample.StreamLength = BYTELEN(4096);
      sample.LoopMode = LOOP::SINGLE;
      sample.Loop1Type = LTYPE::BIDIRECTIONAL;
      sample.Loop1End = SAMPLE(4096);
      audio->Sets.resize(2);
      audio->Sets[1].Channel.resize(1);
      audio->Sets[1].Shadow.resize(1);
      auto &channel = audio->Sets[1].Channel[0];
      channel.SampleHandle = 1;
      channel.Handle = 1 << 16;
      channel.Frequency = 48000;
      channel.Volume = 1;
      channel.State = CHS::PLAYING;
#ifdef AUDIO_WORKER
      struct WorkerScope {
         bool Previous = glAudioWorker;
         WorkerScope() { glAudioWorker = true; }
         ~WorkerScope() { glAudioWorker = Previous; }
      } worker_scope;
#endif
      std::array<float, 400> reference{};
      for (int block : { 200, 1, 7, 31 }) {
         AUDIO_CHECK(SET_Quality(audio, quality) IS ERR::Okay);
         audio->snapshot_quality();
         channel = AudioChannel{};
         channel.SampleHandle = 1;
         channel.Handle = 1 << 16;
         channel.Frequency = 48000;
         channel.Volume = 1;
         channel.State = CHS::PLAYING;
         audio->Sets[1].Shadow[0] = AudioChannel{};
         AUDIO_REQUIRE(fade_in(audio, &channel) IS ERR::Okay);
         std::array<float, 400> output{};
         for (int frame = 0; frame < 200;) {
            if (frame IS 19) AUDIO_CHECK(snd::MixVolume(audio, channel.Handle, 0.2) IS ERR::Okay);
            if (frame IS 37) AUDIO_CHECK(snd::MixPan(audio, channel.Handle, 1) IS ERR::Okay);
            if (frame IS 53) AUDIO_CHECK(snd::MixMute(audio, channel.Handle, 1) IS ERR::Okay);
            if (frame IS 61) AUDIO_CHECK(snd::MixMute(audio, channel.Handle, 0) IS ERR::Okay);
            if (frame IS 67) {
               AUDIO_CHECK(SET_Quality(audio, quality IS 0 ? 80 : 0) IS ERR::Okay);
               audio->snapshot_quality();
            }
            if (frame IS 73 or frame IS 79) AUDIO_CHECK(snd::MixPlay(audio, channel.Handle, 0) IS ERR::Okay);
            int next = 200;
            for (int boundary : { 19, 37, 53, 61, 67, 73, 79 }) if (boundary > frame) {
               next = boundary;
               break;
            }
            const int count = std::min(block, next - frame);
#ifdef AUDIO_WORKER
            if (stream) {
               sample.Prefilled = true;
               sample.EndOfSource = true;
               sample.Ring.Used = sample.Data.size();
            }
#endif
            mix_channel(audio, channel, count, output.data() + frame * 2);
            auto &shadow = audio->Sets[1].Shadow[0];
            if (!shadow.isStopped()) mix_channel(audio, shadow, count, output.data() + frame * 2);
            frame += count;
         }
         if (block IS 200) reference = output;
         else AUDIO_CHECK(std::equal(output.begin(), output.end(), reference.begin()));
         for (int i = 1; i < 200; ++i) for (int c = 0; c < 2; ++c) {
            // A retrigger can overlap two fading voices; each contributes at most its own 0.01 gain step.
            AUDIO_CHECK(std::abs(output[i * 2 + c] - output[(i - 1) * 2 + c]) <= 0.010001f);
         }
         AUDIO_CHECK(audio->Sets[1].Shadow[0].isStopped());
      }
   }
}

static void shadow_command_boundaries(AudioTestContext &Test)
{
   for (int quality : { 0, 80 }) for (bool stream : { false, true }) {
      extAudio *audio;
      AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
      std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
      AUDIO_REQUIRE(SET_Quality(audio, quality) IS ERR::Okay);
      AUDIO_REQUIRE(InitObject(audio) IS ERR::Okay);
      audio->OutputRate = 48000;
      audio->Stereo = true;
      audio->MasterVolume = 1;
      audio->Mute = false;
      audio->snapshot_quality();
      audio->Samples.resize(2);
      auto &sample = audio->Samples[1];
      sample.SampleType = PCM::U8_MONO;
      sample.FrameBytes = 1;
      sample.SampleLength = SAMPLE(4096);
      sample.Data.assign(4096, 192);
      sample.Stream = stream;
      sample.StreamLengthKnown = true;
      sample.StreamLength = BYTELEN(4096);
      sample.LoopMode = LOOP::SINGLE;
      sample.Loop1Type = LTYPE::UNIDIRECTIONAL;
      sample.Loop1End = sample.SampleLength;
      audio->Sets.resize(2);
      audio->Sets[1].Channel.resize(1);
      audio->Sets[1].Shadow.resize(1);
      auto &channel = audio->Sets[1].Channel[0];
      auto &shadow = audio->Sets[1].Shadow[0];
#ifdef AUDIO_WORKER
      struct WorkerScope {
         bool Previous = glAudioWorker;
         WorkerScope() { glAudioWorker = true; }
         ~WorkerScope() { glAudioWorker = Previous; }
      } worker_scope;
#endif
      for (bool release : { false, true }) {
         if (stream and release) continue; // Stream release drains source data rather than creating a shadow.
         channel = AudioChannel{};
         shadow = AudioChannel{};
         channel.Handle = 1 << 16;
         channel.SampleHandle = 1;
         channel.Frequency = 48000;
         channel.Volume = channel.LVolume = channel.RVolume = 1;
         channel.State = CHS::PLAYING;
#ifdef AUDIO_WORKER
         sample.Prefilled = sample.EndOfSource = true;
         sample.Ring.Read = 0;
         sample.Ring.Used = sample.Data.size();
#endif
         float previous[2]{};
         mix_channel(audio, channel, 1, previous);
         AUDIO_CHECK(previous[0] IS 0.5f and previous[1] IS 0.5f);
         if (release) AUDIO_REQUIRE(snd::MixRelease(audio, channel.Handle) IS ERR::Okay);
         else {
            // Three commands at the same boundary: resident -> held -> held, with no render in between.
            for (int retrigger = 0; retrigger < 3; ++retrigger) {
               AUDIO_REQUIRE(snd::MixPlay(audio, channel.Handle, 0) IS ERR::Okay);
               AUDIO_CHECK(shadow.LastLeft IS previous[0] and shadow.LastRight IS previous[1]);
            }
         }
         AUDIO_CHECK(!shadow.isStopped());
#ifdef AUDIO_WORKER
         if (stream) {
            sample.Prefilled = sample.EndOfSource = true;
            sample.Ring.Used = sample.Data.size();
         }
#endif
         float next[2]{};
         mix_channel(audio, channel, 1, next);
         mix_channel(audio, shadow, 1, next);
         AUDIO_CHECK(std::abs(next[0] - previous[0]) <= 0.005001f);
         AUDIO_CHECK(std::abs(next[1] - previous[1]) <= 0.005001f);
         AUDIO_REQUIRE(snd::MixStop(audio, channel.Handle) IS ERR::Okay);
         AUDIO_CHECK(channel.isStopped() and shadow.isStopped());
         float stopped[16]{};
         mix_channel(audio, channel, 8, stopped);
         mix_channel(audio, shadow, 8, stopped);
         AUDIO_CHECK(std::all_of(std::begin(stopped), std::end(stopped), [](float Value) { return Value IS 0; }));
      }
   }
}

static void held_tail_controls(AudioTestContext &Test)
{
   extAudio *audio;
   AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
   std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
   AUDIO_REQUIRE(InitObject(audio) IS ERR::Okay);
   audio->Stereo = true;
   audio->MasterVolume = 0.5;
   audio->Mute = false;
   AudioChannel shadow{};
   shadow.State = CHS::FADE_OUT;
   shadow.HeldFrames = glRampFrames;
   shadow.HeldLeft = 0.5f;
   shadow.HeldRight = -0.5f;
   float output[2]{};
   mix_channel(audio, shadow, 1, output);
   AUDIO_CHECK(output[0] IS 0.25f and output[1] IS -0.25f);
   audio->Mute = true;
   output[0] = output[1] = 0;
   mix_channel(audio, shadow, 1, output);
   AUDIO_CHECK(output[0] IS 0 and output[1] IS 0 and shadow.HeldFrames IS 98);
   audio->Mute = false;
   audio->MasterVolume = 1;
   output[0] = output[1] = 0;
   mix_channel(audio, shadow, 1, output);
   AUDIO_CHECK(std::abs(output[0] - 0.49f) < 1e-7);
   shadow.State = CHS::STOPPED;
   output[0] = output[1] = 0;
   mix_channel(audio, shadow, 1, output);
   AUDIO_CHECK(output[0] IS 0 and output[1] IS 0 and shadow.HeldFrames IS 97);

   // A sub-step gain must remain audible once its short ramp reaches the target.
   audio->OutputRate = 48000;
   audio->snapshot_quality();
   audio->Samples.resize(2);
   auto &sample = audio->Samples[1];
   sample.SampleType = PCM::U8_MONO;
   sample.FrameBytes = 1;
   sample.SampleLength = SAMPLE(8);
   sample.Data.assign(8, 192);
   AudioChannel channel{};
   channel.SampleHandle = 1;
   channel.Frequency = 48000;
   channel.State = CHS::PLAYING;
   channel.Volume = 0.005;
   AUDIO_REQUIRE(set_channel_volume(audio, &channel) IS ERR::Okay);
   float quiet[6]{};
   mix_channel(audio, channel, 3, quiet);
   AUDIO_CHECK(quiet[0] IS 0 and quiet[1] IS 0);
   AUDIO_CHECK(std::abs(quiet[2] - 0.0025f) < 1e-8 and quiet[2] IS quiet[4]);
   AUDIO_CHECK(channel.Position IS 3);
   audio->MasterVolume = std::numeric_limits<double>::denorm_min();
   output[0] = output[1] = 0;
   mix_channel(audio, channel, 1, output);
   AUDIO_CHECK(std::isfinite(channel.LastLeft) and std::isfinite(channel.LastRight));

   // The compatibility switch disables both startup and superseded-voice ramps.
   extAudio *immediate;
   AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &immediate) IS ERR::Okay);
   std::unique_ptr<extAudio, DeleteObject<extAudio>> immediate_owner(immediate);
   AUDIO_REQUIRE(SET_Flags(immediate, 0) IS ERR::Okay);
   AUDIO_REQUIRE(InitObject(immediate) IS ERR::Okay);
   immediate->Stereo = true;
   immediate->Sets.resize(2);
   immediate->Sets[1].Channel.resize(1);
   immediate->Sets[1].Shadow.resize(1);
   auto &voice = immediate->Sets[1].Channel[0];
   voice.Volume = 0.5;
   voice.State = CHS::PLAYING;
   AUDIO_REQUIRE(set_channel_volume(immediate, &voice) IS ERR::Okay);
   AUDIO_REQUIRE(fade_in(immediate, &voice) IS ERR::Okay);
   AUDIO_REQUIRE(fade_out(immediate, 1 << 16) IS ERR::Okay);
   AUDIO_CHECK(voice.LVolume IS 0.5 and voice.RVolume IS 0.5);
   AUDIO_CHECK((voice.Flags & CHF::VOL_RAMP) IS CHF::NIL);
   AUDIO_CHECK(immediate->Sets[1].Shadow[0].isStopped());
}

static void run(AudioTestContext &Test)
{
   struct RestoreStep {
      int Value = glMixStep;
      ~RestoreStep() { set_mix_step(Value); }
   } restore;
   set_mix_step(65536);
   unsigned_mono(Test);
   mono_to_stereo(Test);
   unsigned_stereo(Test);
   volume_accumulation_and_position(Test);
   interpolation(Test);
   pcm_endpoints_and_output_conversion(Test);
   channel_gain_contract(Test);
   ramp_envelopes(Test);
   rapid_ramp_commands(Test);
   held_tail_controls(Test);
   shadow_command_boundaries(Test);
}

} // namespace audio_tests_mixers
