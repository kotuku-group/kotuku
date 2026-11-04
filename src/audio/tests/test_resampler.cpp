#include "sinc_response.h"

// Numerical contracts use the actual dispatch, independent expected waveforms and partitioned renders.
namespace audio_tests_resampler {

static void kernels(AudioTestContext &Test)
{
   constexpr int count = 128;
   for (int fixture = 0; fixture < 4; ++fixture) {
      int16_t source[count];
      for (int i = 0; i < count; ++i) {
         source[i] = fixture IS 0 ? (i IS 64 ? 30000 : 0) : fixture IS 1 ? 12345 :
            fixture IS 2 ? i * 200 - 12000 : int16_t(25000 * std::sin(i * 0.2));
      }
      for (auto mode : { ResamplerMode::NEAREST, ResamplerMode::LINEAR, ResamplerMode::CUBIC, ResamplerMode::SINC }) {
         for (int phase : { 0, 1, 16384, 32768, 65535 }) {
            float reference[count]{};
            for (int block : { count, 1, 7 }) {
               float output[count]{};
               int position = phase;
               for (int i = 0; i < count;) {
                  auto dest = output + i;
                  const int frames = std::min(block, count - i);
                  MixingParams params{ source, position, frames, 1, 1, &dest, 0, count };
                  set_mix_step(65536);
                  position = AudioMixer::dispatch_mix(AudioConfig(false, mode), PCM::S16_MONO, params);
                  i += frames;
               }
               if (block IS count) std::copy_n(output, count, reference);
               for (int i = 0; i < count; ++i) {
                  AUDIO_CHECK(output[i] IS reference[i]);
                  AUDIO_CHECK(std::isfinite(output[i]) and std::abs(output[i]) <= 1);
                  if (!phase) AUDIO_CHECK(std::abs(output[i] - float(source[i]) / 32768) < 6e-8);
                  if (fixture IS 1) AUDIO_CHECK(std::abs(output[i] - float(source[i]) / 32768) < 6e-8);
                  if (fixture IS 2 and i > 0 and i < count - 2 and
                      mode != ResamplerMode::SINC and mode != ResamplerMode::NEAREST) {
                     AUDIO_CHECK(std::abs(output[i] - (source[i] + 200.0 * phase / 65536) / 32768) < 6e-8);
                  }
               }
            }
         }
      }
   }
   // Sweep pass-band frequencies to 0.2 cycles per source frame, with all fractional phases.
   double linear_error = 0, cubic_error = 0;
   for (int frequency = 1; frequency <= 20; ++frequency) for (int i = 2; i < 100; ++i) {
      const double omega = 2 * std::numbers::pi * frequency / 100;
      for (int fraction = 1; fraction < 16; ++fraction) {
         const double phase = fraction / 16.0;
         const double expected = std::sin((i + phase) * omega);
         const double current = std::sin(i * omega), next = std::sin((i + 1) * omega);
         const double linear = current + phase * (next - current);
         const double cubic = resampler_cubic(std::sin((i - 1) * omega), current, next,
            std::sin((i + 2) * omega), phase);
         linear_error += (linear - expected) * (linear - expected);
         cubic_error += (cubic - expected) * (cubic - expected);
      }
   }
   AUDIO_CHECK(cubic_error < linear_error * 0.2);
}

static void transitions(AudioTestContext &Test)
{
   int16_t source[] = { -10000, 20000, -20000, 10000 };
   float reference[96]{};
   for (int block : { 32, 1, 7 }) {
      ResamplerState state;
      float output[96]{};
      set_mix_step(0);
      for (int i = 0; i < 96;) {
         const auto mode = i < 8 ? ResamplerMode::NEAREST : i < 14 ? ResamplerMode::CUBIC :
            i < 20 ? ResamplerMode::SINC : ResamplerMode::LINEAR;
         const int boundary = i < 8 ? 8 : i < 14 ? 14 : i < 20 ? 20 : 96;
         const int frames = std::min(block, boundary - i);
         auto dest = output + i;
         MixingParams params{source, 98304, frames, 1, 1, &dest, 0, 4, 0, 0, LTYPE::NIL, &state};
         AudioMixer::dispatch_mix(AudioConfig(false, mode), PCM::S16_MONO, params);
         i += frames;
      }
      if (block IS 32) std::copy_n(output, 96, reference);
      AUDIO_CHECK(std::equal(output, output + 96, reference));
      AUDIO_CHECK(state.Remaining IS 0 and state.Weights[1] IS 1);
      AUDIO_CHECK(output[51] IS 0 and output[95] IS 0);
      for (int i = 1; i < 96; ++i) AUDIO_CHECK(std::abs(output[i] - output[i-1]) < 0.03);
   }
}

#ifdef AUDIO_WORKER
static void boundaries(AudioTestContext &Test)
{
   for (auto format : { PCM::U8_MONO, PCM::U8_STEREO, PCM::S16_MONO, PCM::S16_STEREO })
   for (bool stereo : { false, true }) for (int quality : { 0, 10, 33, 80 })
   for (int rate : { 1000, 44100, 48000, 96000 }) for (int ratio : { 1, 2, 4 })
   for (auto loop : { LTYPE::NIL, LTYPE::UNIDIRECTIONAL, LTYPE::BIDIRECTIONAL })
   for (int loop_index : { 1, 2 }) {
      extAudio *audio;
      AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
      std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
      SET_Quality(audio, quality);
      AUDIO_REQUIRE(InitObject(audio) IS ERR::Okay);
      audio->Sets.resize(1);
      audio->OutputRate = rate;
      audio->Stereo = stereo;
      audio->MasterVolume = 1;
      audio->Mute = false;
      audio->snapshot_quality();
      audio->Samples.resize(2);
      auto &sample = audio->Samples[1];
      const bool source_stereo = pcm_stereo(format);
      const bool wide = format IS PCM::S16_MONO or format IS PCM::S16_STEREO;
      const int frame_bytes = (source_stereo ? 2 : 1) * (wide ? 2 : 1);
      sample.FrameBytes = frame_bytes;
      sample.SampleType = format;
      sample.SampleLength = SAMPLE(19);
      sample.Loop1Type = loop;
      sample.Loop1Start = SAMPLE(3);
      sample.Loop1End = SAMPLE(19);
      sample.Loop2Type = loop;
      sample.Loop2Start = SAMPLE(3);
      sample.Loop2End = SAMPLE(19);
      sample.Data.resize(19 * frame_bytes);
      for (int i = 0; i < 19 * (source_stereo ? 2 : 1); ++i) {
         if (wide) ((int16_t *)sample.Data.data())[i] = int16_t(24000 * std::sin(i * 0.7));
         else sample.Data[i] = uint8_t(128 + 100 * std::sin(i * 0.7));
      }
      const auto resident_data = sample.Data;
      AudioChannel initial{};
      initial.SampleHandle = 1;
      initial.LoopIndex = loop_index;
      initial.Frequency = rate * ratio / 2;
      initial.State = CHS::PLAYING;
      initial.LVolume = initial.RVolume = 1;
      const int channels = stereo ? 2 : 1;
      std::array<float, 240> reference{};
      AudioChannel expected{};
      for (bool backward : { false, true }) {
         initial.Flags = backward ? CHF::BACKWARD : CHF::NIL;
         initial.Position = backward ? 17 : 0;
         initial.PositionLow = 32768;
         for (int block : { 120, 1, 7 }) {
            auto channel = initial;
            std::array<float, 240> output{};
            for (int i = 0; i < 120; i += block) {
               mix_channel(audio, channel, std::min(block, 120 - i), output.data() + i * channels);
            }
            if (block IS 120) { reference = output; expected = channel; }
            else {
               AUDIO_CHECK(output IS reference);
               AUDIO_CHECK(channel.Position IS expected.Position and channel.PositionLow IS expected.PositionLow);
            }
            for (auto value : output) AUDIO_CHECK(std::isfinite(value) and std::abs(value) <= 2);
         }
         if (backward or loop IS LTYPE::BIDIRECTIONAL) continue; // Stream cursors consume forward only.
         // Materialise logical refill data in a rotated ring: the current frame and future support wrap physically.
         constexpr int available = 512;
         const int stored = loop IS LTYPE::NIL ? 19 : available;
         sample.Data.assign(available * frame_bytes, 0);
         sample.Stream = sample.Prefilled = sample.EndOfSource = true;
         for (int block : { 120, 1, 7 }) {
            sample.Ring.Read = (available - 2) * frame_bytes;
            sample.Ring.Used = stored * frame_bytes;
            sample.HistoryFrames = 0;
            sample.PlayPos = BYTELEN(0);
            for (int i = 0; i < stored; ++i) {
               const int index = i < 19 ? i : 3 + (i - 19) % 16;
               for (int byte = 0; byte < frame_bytes; ++byte) {
                  sample.Data[(sample.Ring.Read + i * frame_bytes + byte) % sample.Data.size()] =
                     resident_data[index * frame_bytes + byte];
               }
            }
            auto channel = initial;
            std::array<float, 240> output{};
            if (quality IS 80) {
               sample.EndOfSource = false;
               sample.Ring.Used = frame_bytes;
               mix_channel(audio, channel, 1, output.data());
               AUDIO_CHECK(output IS (std::array<float, 240>{}));
               AUDIO_CHECK(channel.PositionLow IS initial.PositionLow and !channel.Resampler.Initialised);
               AUDIO_CHECK(sample.HistoryFrames IS 0 and sample.Ring.Used IS size_t(frame_bytes));
               sample.EndOfSource = true;
               sample.Ring.Used = stored * frame_bytes;
            }
            for (int i = 0; i < 120; i += block) {
               mix_channel(audio, channel, std::min(block, 120 - i), output.data() + i * channels);
            }
            AUDIO_CHECK(output IS reference);
         }
         sample.Stream = false;
         sample.Data = resident_data;
      }
   }
}
#endif

static void short_loops(AudioTestContext &Test)
{
   extAudio *audio;
   AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
   std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
   AUDIO_REQUIRE(InitObject(audio) IS ERR::Okay);
   audio->OutputRate = 48000;
   audio->Stereo = false;
   audio->MasterVolume = 1;
   audio->Mute = false;
   audio->Sets.resize(1);
   audio->Samples.resize(2);
   auto &sample = audio->Samples[1];
   sample.SampleType = PCM::S16_MONO;
   sample.FrameBytes = 2;
   for (int length = 1; length <= 4; ++length)
   for (auto loop : { LTYPE::UNIDIRECTIONAL, LTYPE::BIDIRECTIONAL })
   for (int frequency : { 48000, 504000 }) for (int quality : { 0, 10, 33, 80 }) {
      SET_Quality(audio, quality);
      audio->snapshot_quality();
      sample.SampleLength = sample.Loop1End = SAMPLE(length);
      sample.Loop1Start = SAMPLE(0);
      sample.Loop1Type = loop;
      sample.Data.resize(length * 2);
      for (int i = 0; i < length; ++i) ((int16_t *)sample.Data.data())[i] = int16_t(i * 8000 - 12000);
      int16_t expanded[400];
      for (int i = 0; i < 400; ++i) {
         int index = i % (loop IS LTYPE::BIDIRECTIONAL ? 2 * length : length);
         if (index >= length) index = 2 * length - 1 - index;
         expanded[i] = ((int16_t *)sample.Data.data())[index];
      }
      float expected[32]{}, actual[32]{};
      auto dest = expected;
      set_mix_step(int(int64_t(frequency) * 65536 / 48000));
      MixingParams params{expanded, 0, 32, 1, 1, &dest, 0, 400};
      AudioMixer::dispatch_mix(audio->WindowConfig, PCM::S16_MONO, params);
      AudioChannel channel{};
      channel.SampleHandle = 1;
      channel.State = CHS::PLAYING;
      channel.Frequency = frequency;
      channel.LVolume = channel.RVolume = 1;
      mix_channel(audio, channel, 32, actual);
      for (int i = 0; i < 32; ++i) {
         // Draft retains spatial floor sampling, so fractional reverse positions select the lower spatial
         // sample (the next chronological sample). Linear/cubic are invariant under reversing the support.
         const double logical = double(i) * frequency / 48000;
         if (!quality and loop IS LTYPE::BIDIRECTIONAL and std::fmod(logical, 2 * length) >= length) {
            expected[i] = float(expanded[int(std::ceil(logical))]) / 32768;
         }
         AUDIO_CHECK(std::abs(actual[i] - expected[i]) < 6e-8);
      }
      AUDIO_CHECK(!channel.isStopped());
   }
}

static void sample_replacement(AudioTestContext &Test)
{
   for (auto old_loop : { LTYPE::UNIDIRECTIONAL, LTYPE::BIDIRECTIONAL })
   for (auto new_loop : { LTYPE::UNIDIRECTIONAL, LTYPE::BIDIRECTIONAL })
   for (int phase : { 0, 32768 }) {
      extAudio *audio;
      AUDIO_REQUIRE(NewObject(CLASSID::AUDIO, &audio) IS ERR::Okay);
      std::unique_ptr<extAudio, DeleteObject<extAudio>> owner(audio);
      AUDIO_REQUIRE(SET_Quality(audio, 80) IS ERR::Okay);
      AUDIO_REQUIRE(InitObject(audio) IS ERR::Okay);
      audio->OutputRate = 48000;
      audio->Stereo = false;
      audio->MasterVolume = 1;
      audio->Mute = false;
      audio->snapshot_quality();
      audio->Samples.resize(3);
      for (int index = 1; index <= 2; ++index) {
         auto &sample = audio->Samples[index];
         sample.SampleType = PCM::S16_MONO;
         sample.FrameBytes = 2;
         sample.SampleLength = sample.Loop1End = SAMPLE(index IS 1 ? 16 : 17);
         sample.LoopMode = LOOP::SINGLE;
         sample.Loop1Type = index IS 1 ? old_loop : new_loop;
         sample.Data.resize(int64_t(sample.SampleLength) * sample.FrameBytes);
         for (int i = 0; i < sample.SampleLength; ++i) {
            ((int16_t *)sample.Data.data())[i] = int16_t((i + 1) * 1000);
         }
      }
      audio->Sets.resize(2);
      audio->Sets[1].Channel.resize(1);
      auto &channel = audio->Sets[1].Channel[0];
      channel.Handle = 1 << 16;
      channel.SampleHandle = 1;
      channel.Frequency = 48000;
      channel.PositionLow = phase;
      channel.State = CHS::PLAYING;
      channel.LVolume = channel.RVolume = 1;
      float introduction[19]{};
      mix_channel(audio, channel, 19, introduction);
      AUDIO_REQUIRE(channel.LoopTimeline and channel.TimelineOffset != 0);

#ifdef AUDIO_WORKER
      struct WorkerScope {
         bool Previous = glAudioWorker;
         WorkerScope() { glAudioWorker = true; }
         ~WorkerScope() { glAudioWorker = Previous; }
      } worker_scope;
#endif
      const auto previous_offset = channel.TimelineOffset;
      const auto previous_position = channel.Position;
      const auto previous_fraction = channel.PositionLow;
      const auto previous_direction = channel.Flags & CHF::BACKWARD;
      AUDIO_REQUIRE(snd::MixSample(audio, channel.Handle, 1) IS ERR::Okay);
      AUDIO_CHECK(channel.LoopTimeline and channel.TimelineOffset IS previous_offset);
      AUDIO_REQUIRE(snd::MixSample(audio, channel.Handle, 2) IS ERR::Okay);
      AUDIO_CHECK(channel.Position IS previous_position and channel.PositionLow IS previous_fraction);
      AUDIO_CHECK((channel.Flags & CHF::BACKWARD) IS previous_direction);

      // Materialise the replacement's spatial loop independently of the old source's unfolded timeline.
      int16_t expanded[256];
      const int period = new_loop IS LTYPE::BIDIRECTIONAL ? 34 : 17;
      for (int i = 0; i < 256; ++i) {
         int offset = ((i - 96) % period + period) % period;
         if (offset >= 17) offset = period - 1 - offset;
         expanded[i] = int16_t((offset + 1) * 1000);
      }
      float expected[32]{}, actual[32]{};
      auto dest = expected;
      set_mix_step((channel.Flags & CHF::BACKWARD) != CHF::NIL ? -65536 : 65536);
      MixingParams params{expanded, int((96 + channel.Position) * 65536) + channel.PositionLow,
         32, 1, 1, &dest, 0, 256};
      AudioMixer::dispatch_mix(audio->WindowConfig, PCM::S16_MONO, params);
      mix_channel(audio, channel, 32, actual);
      for (int i = 0; i < 32; ++i) AUDIO_CHECK(std::abs(actual[i] - expected[i]) < 6e-8);
   }
}

static void sinc_tones(AudioTestContext &Test)
{
   constexpr double pi = 3.14159265358979323846;
   for (double ratio : { 44100.0 / 48000, 48000.0 / 44100, 2.0, 1.5, 2.7 }) {
      const int step = int(ratio * 65536);
      ratio = double(step) / 65536;
      for (bool stopband : { false, true }) {
         if (stopband and ratio <= 1) continue;
         const double frequency = stopband ? 0.5 / ratio + 0.02 : 0.3 / std::max(1.0, ratio);
         int16_t source[2048];
         for (int i = 0; i < 2048; ++i) source[i] = int16_t(28000 * std::sin(2 * pi * frequency * i));
         double errors[2]{};
         for (int mode = 0; mode < 2; ++mode) {
            float output[512]{};
            auto dest = output;
            set_mix_step(step);
            MixingParams params{source, 128 * 65536 + 12345, 512, 1, 1, &dest, 0, 2048};
            AudioMixer::dispatch_mix(AudioConfig(false, mode ? ResamplerMode::SINC : ResamplerMode::CUBIC),
               PCM::S16_MONO, params);
            for (int i = 0; i < 512; ++i) {
               const double expected = stopband ? 0 : 28000.0 / 32768 *
                  std::sin(2 * pi * frequency * (128 + 12345.0 / 65536 + i * ratio));
               errors[mode] += (output[i] - expected) * (output[i] - expected);
            }
         }
         AUDIO_CHECK(errors[1] < errors[0] * (stopband ? 1e-6 : 0.01));
         AUDIO_CHECK(errors[1] / 512 < 1e-7);
      }
   }
}

static void run(AudioTestContext &Test)
{
   for (double ratio : { 44100.0 / 48000, 48000.0 / 44100, 2.0, 1.5, 2.7 }) {
      const auto response = measure_sinc(ratio);
      AUDIO_CHECK(response.RippleDb < 0.1);
      if (ratio > 1) {
         AUDIO_CHECK(response.StopDb > 60);
         AUDIO_CHECK(response.AliasRatio < 1e-6);
      }
   }
   sinc_tones(Test);
   sample_replacement(Test);
   kernels(Test);
   short_loops(Test);
   transitions(Test);
#ifdef AUDIO_WORKER
   boundaries(Test);
#endif
   set_mix_step(65536);
}
}
