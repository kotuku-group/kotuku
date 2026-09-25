
const double RAMPSPEED = 0.01;  // Default ramping speed - volume steps per output sample.  Keeping this value very low prevents clicks from occurring

#include <type_traits>  // For SFINAE and template metaprogramming
#include "mixer_dispatch.h"
#include "audio_batch.h"

static void filter_float_mono(extAudio *, float *, int);
static void filter_float_stereo(extAudio *, float *, int);
static void mix_channel(extAudio *, AudioChannel &, int, APTR);
static ERR mix_data(extAudio *, int, APTR);
static ERR process_commands(extAudio *, SAMPLE);

inline bool adjust_volume_ramp(double &current, double target, double ramp_speed) {
   if (current < target) {
      current += ramp_speed;
      if (current >= target) {
         current = target;
         return false;
      }
      return true;
   }
   else if (current > target) {
      current -= ramp_speed;
      if (current <= target) {
         current = target;
         return false;
      }
      return true;
   }
   return false;
}

// Template-based sample format traits for compile-time optimization
template<SFM format>
struct SampleFormatTraits;

template<>
struct SampleFormatTraits<SFM::S16_BIT_STEREO> {
   using type = int16_t;
   static constexpr int size = sizeof(int16_t) * 2;
   static constexpr double conversion = 1.0 / 32767.0;
   static constexpr bool is_stereo = true;
};

template<>
struct SampleFormatTraits<SFM::S16_BIT_MONO> {
   using type = int16_t;
   static constexpr int size = sizeof(int16_t);
   static constexpr double conversion = 1.0 / 32767.0;
   static constexpr bool is_stereo = false;
};

template<>
struct SampleFormatTraits<SFM::U8_BIT_STEREO> {
   using type = uint8_t;
   static constexpr int size = sizeof(uint8_t) * 2;
   static constexpr double conversion = 1.0 / 127.0;
   static constexpr bool is_stereo = true;
};

template<>
struct SampleFormatTraits<SFM::U8_BIT_MONO> {
   using type = uint8_t;
   static constexpr int size = sizeof(uint8_t);
   static constexpr double conversion = 1.0 / 127.0;
   static constexpr bool is_stereo = false;
};

// Constexpr clamping functions for compile-time optimization

template<typename T>
constexpr T clamp_sample(T value, T min_val, T max_val) noexcept {
   return (value < min_val) ? min_val : ((value > max_val) ? max_val : value);
}

//********************************************************************************************************************
// SIMD-aware conversion with template specialization and loop unrolling

template<typename OutputType, typename InputType = float, std::size_t UnrollFactor = 4>
void convert_samples(const InputType* input, int count, OutputType* output) {
   static_assert(std::is_arithmetic_v<OutputType>, "Output type must be arithmetic");
   static_assert(UnrollFactor > 0 and UnrollFactor <= 8, "Unroll factor must be 1-8");

   const InputType* end = input + count;
   const InputType* unroll_end = input + (count - count % UnrollFactor);

   if constexpr (std::is_same_v<OutputType, uint8_t>) {
      // 8-bit conversion with loop unrolling
      while (input < unroll_end) {
         for (std::size_t i = 0; i < UnrollFactor; ++i) {
            const int sample = int(input[i]) >> 8;
            output[i] = uint8_t(128 + clamp_sample(sample, int(-128), int(127)));
         }
         input += UnrollFactor;
         output += UnrollFactor;
      }

      // Handle remaining samples
      while (input < end) {
         const int sample = int(*input) >> 8;
         *output = uint8_t(128 + clamp_sample(sample, int(-128), int(127)));
         ++input; ++output;
      }
   }
   else if constexpr (std::is_same_v<OutputType, int16_t>) {
      // 16-bit conversion with loop unrolling
      while (input < unroll_end) {
         for (std::size_t i = 0; i < UnrollFactor; ++i) {
            const int sample = int(input[i]);
            output[i] = int16_t(clamp_sample(sample, int(-32768), int(32767)));
         }
         input += UnrollFactor;
         output += UnrollFactor;
      }

      while (input < end) {
         const int sample = int(*input);
         *output = int16_t(clamp_sample(sample, int(-32768), int(32767)));
         ++input; ++output;
      }
   }
   else if constexpr (std::is_same_v<OutputType, float>) {
      // Float conversion with vectorization hints
      while (input < unroll_end) {
         for (std::size_t i = 0; i < UnrollFactor; ++i) {
            output[i] = clamp_sample(float(input[i]), -1.0f, 1.0f);
         }
         input += UnrollFactor;
         output += UnrollFactor;
      }

      while (input < end) {
         *output = clamp_sample(float(*input), -1.0f, 1.0f);
         ++input; ++output;
      }
   }
   else {
      static_assert(std::is_same_v<OutputType, void>, "Unsupported output type");
   }
}

//********************************************************************************************************************

static void audio_stopped_event(extAudio &Audio, int SampleHandle)
{
   FUNCTION callback;
   {
      std::lock_guard lock(Audio.MixerMutex);
      if (size_t(SampleHandle) >= Audio.Samples.size()) return;
      auto &sample = Audio.Samples[SampleHandle];
      if (!sample.OnStop.defined()) return;
      if (sample.OnStop.stale()) {
         release_audio_callback(sample.OnStop);
         return;
      }
      callback = sample.OnStop;
      callback.pin();
   }

   if (callback.isC()) {
      kt::SwitchContext context(callback.Context);
      auto routine = (void (*)(extAudio *, int, APTR))callback.Routine;
      routine(&Audio, SampleHandle, callback.Meta);
   }
   else if (callback.isScript()) {
      sc::Call(callback, std::to_array<ScriptArg>({ { "Audio", &Audio, FD_OBJECTPTR }, { "Handle", SampleHandle } }));
   }
   callback.unpin();
}

//********************************************************************************************************************
// The callback must return the number of bytes written to the buffer.

static BYTELEN fill_stream_buffer(int Handle, AudioSample &Sample, int Offset)
{
   if (Sample.Callback.stale()) return BYTELEN(0);

   if (Sample.Callback.isC()) {
      kt::SwitchContext context(Sample.Callback.Context);
      auto routine = (BYTELEN (*)(int, int, uint8_t *, int, APTR))Sample.Callback.Routine;
      return routine(Handle, Offset, Sample.Data.data(), Sample.SampleLength<<sample_shift(Sample.SampleType), Sample.Callback.Meta);
   }
   else if (Sample.Callback.isScript()) {
      std::span span(Sample.Data.data(), Sample.SampleLength<<sample_shift(Sample.SampleType));
      const auto args = std::to_array<ScriptArg>({
         { "Handle", Handle },
         { "Offset", Offset },
         { "Buffer", &span, FDF_SPAN|FD_BYTE }
      });

      ERR result;
      auto script = (objScript *)Sample.Callback.Context;
      script->Results.clear();
      if (sc::Call(Sample.Callback, args, result) IS ERR::Okay and result IS ERR::Okay) {
         std::span<std::string> results;
         if (script->getResults(results) IS ERR::Okay and !results.empty()) {
            return BYTELEN(std::clamp<long>(strtol(results[0].c_str(), nullptr, 10), 0, span.size()));
         }
      }
      return BYTELEN(0);
   }

   return BYTELEN(0);
}

//********************************************************************************************************************

[[maybe_unused]] static ERR get_mix_amount(extAudio *Self, SAMPLE *MixLeft)
{
   auto ml = SAMPLE(0x7fffffff);
   for (int i=1; i < (int)Self->Sets.size(); i++) {
      if ((Self->Sets[i].MixLeft > 0) and (Self->Sets[i].MixLeft < ml)) {
         ml = Self->Sets[i].MixLeft;
      }
   }

   *MixLeft = ml;
   return ERR::Okay;
}

//********************************************************************************************************************
// Defines the L/RVolume and Ramping values for an AudioChannel.  These values are derived from the Volume and Pan.

static ERR set_channel_volume(extAudio *Self, AudioChannel *Channel)
{
   AudioLog log(__FUNCTION__);

   if ((!Self) or (!Channel)) return log.warning(ERR::NullArgs);

   if (Channel->Volume > 1.0) Channel->Volume = 1.0;
   else if (Channel->Volume < 0) Channel->Volume = 0;

   if (Channel->Pan < -1.0) Channel->Pan = -1.0;
   else if (Channel->Pan > 1.0) Channel->Pan = 1.0;

   // Convert the volume into left/right volume parameters

   double leftvol, rightvol;
   if ((Channel->Flags & CHF::MUTE) != CHF::NIL) {
      leftvol  = 0;
      rightvol = 0;
   }
   else {
      leftvol  = Channel->Volume;
      rightvol = Channel->Volume;

      if (!Self->Stereo);
      else if (Channel->Pan < 0) rightvol = (Channel->Volume * (1.0 + Channel->Pan));
      else if (Channel->Pan > 0) leftvol  = (Channel->Volume * (1.0 - Channel->Pan));
   }

   // Start volume ramping if necessary

   Channel->Flags &= ~CHF::VOL_RAMP;
   if (((Self->Flags & ADF::OVER_SAMPLING) != ADF::NIL) and ((Self->Flags & ADF::VOL_RAMPING) != ADF::NIL)) {
      if ((Channel->LVolume != leftvol) or (Channel->LVolumeTarget != leftvol)) {
         Channel->Flags |= CHF::VOL_RAMP;
         Channel->LVolumeTarget = leftvol;
      }

      if ((Channel->RVolume != rightvol) or (Channel->RVolumeTarget != rightvol)) {
         Channel->Flags |= CHF::VOL_RAMP;
         Channel->RVolumeTarget = rightvol;
      }
   }
   else {
      Channel->LVolume       = leftvol;
      Channel->LVolumeTarget = leftvol;
      Channel->RVolume       = rightvol;
      Channel->RVolumeTarget = rightvol;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Execute one complete batch per channel set at its next mixer update boundary.

[[maybe_unused]] static ERR process_commands(extAudio *Self, SAMPLE Elements)
{
   for (int index=1; index < (int)Self->Sets.size(); index++) {
      Self->Sets[index].MixLeft -= Elements;
      if (Self->Sets[index].MixLeft <= 0) {
         // Reset the amount of mixing elements left and execute the next set of channel commands

         Self->Sets[index].MixLeft = Self->MixLeft(Self->Sets[index].UpdateRate);

         if (Self->Sets[index].Commands.empty()) continue;

         execute_next_audio_batch(Self->Sets[index], [Self](const AudioCommand &Command) {
            #ifdef AUDIO_WORKER
            execute_audio_command(Self, Command);
            #else
            switch (Command.CommandID) {
               case CMD::CONTINUE: snd::MixContinue(Self, Command.Handle); break;
               case CMD::MUTE: snd::MixMute(Self, Command.Handle, std::get<bool>(Command.Data)); break;
               case CMD::PLAY: snd::MixPlay(Self, Command.Handle, std::get<int>(Command.Data)); break;
               case CMD::FREQUENCY: snd::MixFrequency(Self, Command.Handle, std::get<int>(Command.Data)); break;
               case CMD::PAN: snd::MixPan(Self, Command.Handle, std::get<double>(Command.Data)); break;
               case CMD::RATE: snd::MixRate(Self, Command.Handle, std::get<int>(Command.Data)); break;
               case CMD::SAMPLE: snd::MixSample(Self, Command.Handle, std::get<int>(Command.Data)); break;
               case CMD::VOLUME: snd::MixVolume(Self, Command.Handle, std::get<double>(Command.Data)); break;
               case CMD::STOP: snd::MixStop(Self, Command.Handle); break;
               case CMD::STOP_LOOPING: snd::MixStopLoop(Self, Command.Handle); break;
               default: break;
            }
            #endif
         });
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

// Template-based conversion functions (legacy wrappers for compatibility)

static void convert_float8(float *buf, int TotalSamples, uint8_t *dest) {
   convert_samples<uint8_t>(buf, TotalSamples, dest);
}

static void convert_float16(float *buf, int TotalSamples, int16_t *dest) {
   convert_samples<int16_t>(buf, TotalSamples, dest);
}

static void convert_float(float *buf, int TotalSamples, float *dest) {
   convert_samples<float>(buf, TotalSamples, dest);
}

//********************************************************************************************************************
// Return a maximum of 32k-1 samples to prevent overflow problems

// Template dispatch table for loop types with compile-time optimization
template<LTYPE loop_type>
static int samples_until_end_impl(int position, int sample_length, int lp_start, int lp_end,
   bool over_sampling, bool is_backward, int &next_offset)
{
   next_offset = 1;

   if constexpr (loop_type IS LTYPE::UNIDIRECTIONAL) {
      if (over_sampling) {
         if ((position + 1) < lp_end) {
            return (lp_end - 1) - position;
         }
         else {
            next_offset = lp_start - position;
            return lp_end - position;
         }
      }
      else return lp_end - position;
   }
   else if constexpr (loop_type IS LTYPE::BIDIRECTIONAL) {
      if (is_backward) {
         if (over_sampling) {
            if (position IS (lp_end - 1)) {
               next_offset = 0;
               return 1;
            }
            else return position - lp_start;
         }
         else return position - lp_start;
      }
      else if (over_sampling) {
         if ((position + 1) < lp_end) return (lp_end - 1) - position;
         else {
            next_offset = 0;
            return 1;
         }
      }
      else return lp_end - position;
   }
   else { // Default/no loop
      if (over_sampling) {
         if ((position + 1) < sample_length) return (sample_length - 1) - position;
         else {
            next_offset = 0;
            return sample_length - position;
         }
      }
      else return sample_length - position;
   }
}

//********************************************************************************************************************

static int samples_until_end(extAudio *Self, AudioChannel &Channel, int &NextOffset)
{
   AudioLog log(__FUNCTION__);

   auto &sample = Self->Samples[Channel.SampleHandle];

   // Use structured bindings and conditional operator for cleaner loop parameter selection
   const auto [lp_start, lp_end, lp_type] = (Channel.LoopIndex IS 2) ?
      std::make_tuple(sample.Loop2Start, sample.Loop2End, sample.Loop2Type) :
      std::make_tuple(sample.Loop1Start, sample.Loop1End, sample.Loop1Type);

   const bool over_sampling = (Self->Flags & ADF::OVER_SAMPLING) != ADF::NIL;
   const bool is_backward = (Channel.Flags & CHF::BACKWARD) != CHF::NIL;

   // Template dispatch for compile-time optimization based on loop type
   int num;
   switch (lp_type) {
      case LTYPE::UNIDIRECTIONAL:
         num = samples_until_end_impl<LTYPE::UNIDIRECTIONAL>(
            Channel.Position, sample.SampleLength, lp_start, lp_end, over_sampling, is_backward, NextOffset);
         break;
      case LTYPE::BIDIRECTIONAL:
         num = samples_until_end_impl<LTYPE::BIDIRECTIONAL>(
            Channel.Position, sample.SampleLength, lp_start, lp_end, over_sampling, is_backward, NextOffset);
         break;
      default: // Handle all other loop types (SINGLE, etc.) using default template behavior
         // Use a compile-time value that triggers the else branch in the template
         if constexpr (true) {
            if (over_sampling) {
               if ((Channel.Position + 1) < sample.SampleLength) {
                  num = (sample.SampleLength - 1) - Channel.Position;
               }
               else {
                  NextOffset = 0;
                  num = sample.SampleLength - Channel.Position;
               }
            }
            else num = sample.SampleLength - Channel.Position;
         }
         break;
   }

   int result;
   if (num > 0x7FFF) result = 0x7FFF0000; // 16.16 fixed point
   else result = ((num << 16) - Channel.PositionLow);

   if (result < 0) {
      log.warning("Computed invalid SUE value of %d", result);
      return 0;
   }
   else return result;
}

//********************************************************************************************************************

static bool amiga_change(extAudio *Self, AudioChannel &Channel)
{
   // A sample end or sample loop end has been reached, the sample has been changed, and both old and new samples use
   // Amiga compatible looping - handle Amiga Loop Emulation sample change

   if (Channel.SampleHandle > 1) Channel.SampleHandle--;
   auto &sample = Self->Samples[Channel.SampleHandle];
   Channel.Flags &= ~CHF::CHANGED;

   if (sample.LoopMode IS LOOP::AMIGA) {
      // Looping - start playback from loop beginning
      Channel.Position    = sample.Loop1Start;
      Channel.PositionLow = 0;
      return false;
   }

   // Not looping - finish the sample
   Self->finish(Channel, true);
   return true;
}

//********************************************************************************************************************

static bool handle_sample_end(extAudio *Self, AudioChannel &Channel)
{
   AudioLog log(__FUNCTION__);
   int lp_start, lp_end;
   LTYPE lp_type;

   auto &sample = Self->Samples[Channel.SampleHandle];

   if (Channel.LoopIndex IS 2) {
      lp_start = sample.Loop2Start;
      lp_end   = sample.Loop2End;
      lp_type  = sample.Loop2Type;
   }
   else {
      lp_start = sample.Loop1Start;
      lp_end   = sample.Loop1End;
      lp_type  = sample.Loop1Type;
   }

   if (lp_type IS LTYPE::NIL) { // No loop - did we reach sample end?
      if (Channel.Position >= sample.SampleLength) {
         auto &prev_sample = Self->Samples[Channel.SampleHandle-1];
         if (((Channel.Flags & CHF::CHANGED) != CHF::NIL) and
             ((sample.LoopMode IS LOOP::AMIGA) or (sample.LoopMode IS LOOP::AMIGA_NONE)) and
             ((prev_sample.LoopMode IS LOOP::AMIGA) or (prev_sample.LoopMode IS LOOP::AMIGA_NONE))) {
            return amiga_change(Self, Channel);
         }

         // No sample change - we are finished
         Self->finish(Channel, true);
         return true;
      }
      else return false;
   }

   if ((Channel.Flags & CHF::BACKWARD) != CHF::NIL) {
      // Going backwards - did we reach loop start? (signed comparison takes care of possible wraparound)
      if ((Channel.Position < lp_start) or ((Channel.Position IS lp_start) and (Channel.PositionLow IS 0)) ) {
         Channel.Flags &= ~CHF::BACKWARD;
         int n = ((lp_start - Channel.Position) << 16) - Channel.PositionLow - 1;
         // -1 is compensation for the fudge factor at loop end, see below
         Channel.Position = lp_start + (n>>16);
         Channel.PositionLow = n & 0xffff;

         // Don't die on overshort loops
         if (Channel.Position >= lp_end) {
            Channel.Position = lp_start;
            return true;
         }
      }
   }
   else if (Channel.Position >= lp_end) { // Going forward - did we reach loop end?
      if (sample.Stream) {
         // Read the next set of stream data into our sample buffer
         BYTELEN bytes_read = fill_stream_buffer(Channel.SampleHandle, sample, -1);
         sample.BufferedLength = bytes_read;
         auto buffer_len = sample.SampleLength<<sample_shift(sample.SampleType);
         if (bytes_read < buffer_len) {
            clearmem(sample.Data.data() + bytes_read, buffer_len - bytes_read);
         }

         if ((bytes_read <= 0) or (sample.PlayPos >= sample.StreamLength)) {
            // Loop back to the beginning if the client has defined a loop.  Otherwise finish.
            if (sample.Loop2Type != LTYPE::NIL) {
               sample.PlayPos = BYTELEN(0);
               sample.BufferedLength = BYTELEN(0);
            }
            else Self->finish(Channel, true);
         }
         else sample.PlayPos += bytes_read;
      }

      // Check for ALE sample change

      auto &prev_sample = Self->Samples[Channel.SampleHandle-1];

      if (((Channel.Flags & CHF::CHANGED) != CHF::NIL) and
          ((sample.LoopMode IS LOOP::AMIGA) or (sample.LoopMode IS LOOP::AMIGA_NONE)) and
          ((prev_sample.LoopMode IS LOOP::AMIGA) or (prev_sample.LoopMode IS LOOP::AMIGA_NONE))) {
         return amiga_change(Self, Channel);
      }

      // Go to the second loop if the sound has been released

      if ((Channel.LoopIndex IS 1) and (Channel.State IS CHS::RELEASED)) {
         Channel.LoopIndex = 2;
         return false;
      }

      if (lp_type IS LTYPE::BIDIRECTIONAL ) {
         // Bidirectional loop - change direction
         Channel.Flags |= CHF::BACKWARD;
         int n = ((Channel.Position - lp_end) << 16) + Channel.PositionLow + 1;

         // +1 is a fudge factor to make sure we'll access the correct samples all the time - a similar adjustment is
         // also done at the other end of the loop. This screws up interpolation a little when sample rate IS mixing
         // rate, but little enough that it can't be heard.

         if (lp_end < 0x10000) {
            Channel.Position    = ((lp_end << 16) - n)>>16;
            Channel.PositionLow = ((lp_end << 16) - n) & 0xffff;
         }
         else {
            Channel.Position    = ((0xffff0000 - n)>>16) + (lp_end - 0xffff);
            Channel.PositionLow = (0xffff0000 - n) & 0xffff;
         }

         if (Channel.Position <= lp_start) { // Don't die on overshort loops
            Channel.Position = lp_end;
            return true;
         }
         return false;
      }
      else { // Unidirectional loop - just loop to the beginning
         Channel.Position = lp_start + (Channel.Position - lp_end);

         if (Channel.Position >= lp_end) { // Don't die on overshort loops
            Channel.Position = lp_start;
            return true;
         }

         return false;
      }
   }

   return false;
}

//********************************************************************************************************************
// Main entry point for mixing sound data to destination

[[maybe_unused]] static ERR mix_data(extAudio *Self, int Elements, APTR Dest)
{
   AudioLog log(__FUNCTION__);

   while (Elements > 0) {
      // Mix only as much as we can fit in our mixing buffer

      auto window = (Elements > Self->MixElements) ? Self->MixElements : Elements;

      // Clear the mix buffer, then mix all channels to the buffer

      int window_size = sizeof(float) * (Self->Stereo ? (window<<1) : window);
      clearmem(Self->MixBuffer.data(), window_size);

      for (auto n=1; n < (int)Self->Sets.size(); n++) {
         auto &set = Self->Sets[n];
         const bool effects = set.Effects and !set.Effects->Effects.empty();
         auto buffer = effects ? set.ScratchBuffer.data() : Self->MixBuffer.data();
         if (effects) clearmem(buffer, window_size);

         for (auto &c : set.Channel) {
            if (c.active()) mix_channel(Self, c, window, buffer);
         }

         for (auto &c : set.Shadow) {
            if (c.active()) mix_channel(Self, c, window, buffer);
         }

         if (effects) {
            process_effects(*set.Effects, buffer, window);
            for (int i = 0; i < window_size / int(sizeof(float)); ++i) Self->MixBuffer[i] += buffer[i];
         }
      }

      process_effects(*Self->GlobalEffects, Self->MixBuffer.data(), window);

      // Do optional post-processing

      if ((Self->Flags & (ADF::FILTER_LOW|ADF::FILTER_HIGH)) != ADF::NIL) {
         if (Self->Stereo) filter_float_stereo(Self, Self->MixBuffer.data(), window);
         else filter_float_mono(Self, Self->MixBuffer.data(), window);
      }

      // Convert the floating point data to the correct output format

      if (Self->BitDepth IS 32) { // Presumes a floating point target identical to our own
         convert_float(Self->MixBuffer.data(), (Self->Stereo) ? window<<1 : window, (float *)Dest);
      }
      else if (Self->BitDepth IS 24) {

      }
      else if (Self->BitDepth IS 16) {
         convert_float16(Self->MixBuffer.data(), (Self->Stereo) ? window<<1 : window, (int16_t *)Dest);
      }
      else convert_float8(Self->MixBuffer.data(),  (Self->Stereo) ? window<<1 : window, (uint8_t *)Dest);

      Dest = ((uint8_t *)Dest) + (window * Self->DriverBitSize);
      Elements -= window;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

#ifdef AUDIO_WORKER
// Consume only complete source frames.  Missing source data leaves this channel silent without advancing it.
static void mix_stream(extAudio *Self, AudioChannel &Channel, AudioSample &Sample, int TotalSamples, float *Dest)
{
   const int frame_bytes = 1 << sample_shift(Sample.SampleType);
   const int channels = Self->Stereo ? 2 : 1;
   const int64_t step = (int64_t(Channel.Frequency) << 16) / Self->OutputRate;
   if (step <= 0 or !Sample.Prefilled) return;
   for (int i = 0; i < TotalSamples; ++i) {
      const int64_t position = Channel.PositionLow + step;
      const size_t advance = size_t(position >> 16) * frame_bytes;
      const size_t needed = std::max(size_t(frame_bytes), advance);

      if (Sample.Ring.Used < needed and !Sample.EndOfSource) {
         if (!Sample.Starved) { ++Self->Starvations; Sample.Starved = true; }
         request_stream(Self, Sample);
         return;
      }

      if (!Sample.Ring.Used) {
         if (Sample.EndOfSource) Self->finish(Channel, true);
         else request_stream(Self, Sample);
         return;
      }

      // A two-frame window makes interpolation safe even across the ring boundary or at EOF.

      alignas(4) uint8_t frames[8];
      for (int byte = 0; byte < frame_bytes; ++byte) {
         frames[byte] = Sample.Data[(Sample.Ring.Read + byte) % Sample.Data.size()];
         frames[frame_bytes + byte] = Sample.Ring.Used >= size_t(frame_bytes * 2) ?
            Sample.Data[(Sample.Ring.Read + frame_bytes + byte) % Sample.Data.size()] : frames[byte];
      }

      const bool source_stereo = Sample.SampleType IS SFM::U8_BIT_STEREO or
         Sample.SampleType IS SFM::S16_BIT_STEREO;
      double volume = Self->Mute ? 0 : Self->MasterVolume * ((!Self->Stereo and source_stereo) ? 0.5 : 1.0);
      if (Self->BitDepth IS 32) volume /= Sample.SampleType IS SFM::S16_BIT_MONO or
         Sample.SampleType IS SFM::S16_BIT_STEREO ? 32767.0 : 127.0;
      auto dest = Dest + i * channels;
      set_mix_step(int(step));

      MixingParams params = {
         .src = frames, .src_pos = Channel.PositionLow, .total_samples = 1, .next_sample_offset = 1,
         .left_vol = float(volume * Channel.LVolume), .right_vol = float(volume * Channel.RVolume),
         .mix_dest = &dest
      };

      AudioMixer::dispatch_mix(Self->MixConfig, Sample.SampleType, params);
      if ((Channel.Flags & CHF::VOL_RAMP) != CHF::NIL) {
         const bool left = adjust_volume_ramp(Channel.LVolume, Channel.LVolumeTarget, RAMPSPEED);
         const bool right = adjust_volume_ramp(Channel.RVolume, Channel.RVolumeTarget, RAMPSPEED);
         if (!left and !right) Channel.Flags &= ~CHF::VOL_RAMP;
      }

      const size_t consumed = std::min(advance, Sample.Ring.Used);
      Sample.Ring.consume(consumed, Sample.Data.size(), frame_bytes);
      int64_t play_pos = int64_t(Sample.PlayPos) + consumed;
      if (Sample.Loop2Type != LTYPE::NIL and play_pos >= Sample.StreamLength) {
         const int start = Sample.Loop2Start << sample_shift(Sample.SampleType);
         if (Sample.StreamLength > start) {
            play_pos = start + (play_pos - Sample.StreamLength) % (Sample.StreamLength - start);
         }
         else Sample.Loop2Type = LTYPE::NIL;
      }

      Sample.PlayPos = BYTELEN(play_pos);
      Sample.BufferedLength = BYTELEN(Sample.Ring.Used);
      Channel.PositionLow = position & 0xffff;
      if (Sample.Ring.Used <= Sample.Data.size() / 2) request_stream(Self, Sample);
   }
}
#endif

static void mix_channel(extAudio *Self, AudioChannel &Channel, int TotalSamples, APTR Dest)
{
   AudioLog log(__FUNCTION__);

   auto &sample = Self->Samples[Channel.SampleHandle];

   // Check that we have something to mix

   if ((!Channel.Frequency) or (sample.Data.empty()) or (sample.SampleLength <= 0)) return;

#ifdef AUDIO_WORKER
   if (sample.Stream) {
      if (!Channel.isStopped()) mix_stream(Self, Channel, sample, TotalSamples, (float *)Dest);
      return;
   }
#endif

   // Calculate resampling step (16.16 fixed point)

   int step = ((int64_t(Channel.Frequency / Self->OutputRate) << 16) + (int64_t(Channel.Frequency % Self->OutputRate) << 16) / Self->OutputRate);

   // Advanced template metaprogramming: constexpr format dispatch table
   // This eliminates runtime switches with compile-time lookups
   constexpr auto format_dispatch = [](SFM format) constexpr -> std::tuple<int, double, bool> {
      // Using constexpr if-else chain for optimal code generation
      if (format IS SFM::S16_BIT_STEREO) {
         return {SampleFormatTraits<SFM::S16_BIT_STEREO>::size,
                 SampleFormatTraits<SFM::S16_BIT_STEREO>::conversion,
                 SampleFormatTraits<SFM::S16_BIT_STEREO>::is_stereo};
      }
      else if (format IS SFM::S16_BIT_MONO) {
         return {SampleFormatTraits<SFM::S16_BIT_MONO>::size,
                 SampleFormatTraits<SFM::S16_BIT_MONO>::conversion,
                 SampleFormatTraits<SFM::S16_BIT_MONO>::is_stereo};
      }
      else if (format IS SFM::U8_BIT_STEREO) {
         return {SampleFormatTraits<SFM::U8_BIT_STEREO>::size,
                 SampleFormatTraits<SFM::U8_BIT_STEREO>::conversion,
                 SampleFormatTraits<SFM::U8_BIT_STEREO>::is_stereo};
      }
      else {
         return {SampleFormatTraits<SFM::U8_BIT_MONO>::size,
                 SampleFormatTraits<SFM::U8_BIT_MONO>::conversion,
                 SampleFormatTraits<SFM::U8_BIT_MONO>::is_stereo};
      }
   };

   const auto [sample_size, conversion, sample_is_stereo] = format_dispatch(sample.SampleType);

   // Calculate stereo multiplier using template information
   const double stereo_mul = (!Self->Stereo and sample_is_stereo) ? 0.5 : 1.0;
   double mastervol = Self->Mute ? 0 : Self->MasterVolume * stereo_mul;

   if (Self->BitDepth IS 32) {
      // If our hardware output format is floating point, the values need to range from -1.0 to 1.0.
      // The application of the conversion value to mastervol allows us to achieve this optimally
      // and we won't need to apply any further conversions.
      mastervol *= conversion;
   }

   float *mix_dest = (float *)Dest;
   while (TotalSamples > 0) {
      if (Channel.isStopped()) return;

      int next_offset = 0;
      int sue = samples_until_end(Self, Channel, next_offset);

      // Calculate the number of destination samples (note rounding)

      int mixUntilEnd = sue / step;
      if (sue % step) mixUntilEnd++;

      int mix_now = (mixUntilEnd > TotalSamples) ? TotalSamples : mixUntilEnd;
      TotalSamples -= mix_now;

      if (mix_now > 0) {
         if (Channel.PositionLow < 0) { // Sanity check
            log.warning("Detected invalid PositionLow value of %d", Channel.PositionLow);
            return;
         }

         int mix_pos = Channel.PositionLow;
         uint8_t *MixSample = sample.Data.data() + (sample_size * Channel.Position); // source of sample data to mix into destination

         // Thread-safe: pass step direction as parameter instead of using global

         const int mix_step = ((Channel.Flags & CHF::BACKWARD) != CHF::NIL) ? -step : step;
         set_mix_step(mix_step);

         // If volume ramping is enabled, mix one sample element at a time and adjust volume by RAMPSPEED.
         // Using helper function to reduce code duplication

         while (((Channel.Flags & CHF::VOL_RAMP) != CHF::NIL) and (mix_now > 0)) {
            MixingParams params = {
               .src = MixSample,
               .src_pos = mix_pos,
               .total_samples = 1,
               .next_sample_offset = next_offset,
               .left_vol = float(mastervol * Channel.LVolume),
               .right_vol = float(mastervol * Channel.RVolume),
               .mix_dest = &mix_dest
            };

            mix_pos = AudioMixer::dispatch_mix(Self->MixConfig, sample.SampleType, params);
            mix_now--;

            // Use helper function for cleaner volume ramping logic
            const bool left_ramping = adjust_volume_ramp(Channel.LVolume, Channel.LVolumeTarget, RAMPSPEED);
            const bool right_ramping = adjust_volume_ramp(Channel.RVolume, Channel.RVolumeTarget, RAMPSPEED);

            if (!left_ramping and !right_ramping) {
               Channel.Flags &= ~CHF::VOL_RAMP;
            }
         }

         if ((Channel.LVolume <= 0.01) and (Channel.RVolume <= 0.01)) {
            // If the volume is zero we can just increment the position and not mix anything
            mix_pos += mix_now * mix_step;
            if (Channel.State IS CHS::FADE_OUT) {
               Self->finish(Channel, true);
               Channel.Flags &= ~CHF::VOL_RAMP;
            }
         }
         else {
            // Ensure proper alignment and do all mixing if next_offset != 1

            int num;
            if ((mix_now > 0) and ((next_offset != 1) or ((((MAXINT)mix_dest) % 1) != 0))) {
               if (next_offset IS 1) {
                  num = 1 - (((MAXINT)mix_dest) % 1);
                  if (num > mix_now) num = mix_now;
               }
               else num = mix_now;

               MixingParams params = {
                  .src = MixSample,
                  .src_pos = mix_pos,
                  .total_samples = num,
                  .next_sample_offset = next_offset,
                  .left_vol = float(mastervol * Channel.LVolume),
                  .right_vol = float(mastervol * Channel.RVolume),
                  .mix_dest = &mix_dest
               };

               mix_pos = AudioMixer::dispatch_mix(Self->MixConfig, sample.SampleType, params);
               mix_now -= num;
            }

            if (mix_now > 0) { // Main mixing loop
               MixingParams params = {
                  .src = MixSample,
                  .src_pos = mix_pos,
                  .total_samples = mix_now,
                  .next_sample_offset = 1,
                  .left_vol = float(mastervol * Channel.LVolume),
                  .right_vol = float(mastervol * Channel.RVolume),
                  .mix_dest = &mix_dest
               };

               mix_pos = AudioMixer::dispatch_mix(Self->MixConfig, sample.SampleType, params);
            }
         }

         // Save state back to channel structure
         Channel.PositionLow = mix_pos & 0xffff;
         Channel.Position = (mix_pos>>16) + ((MixSample - sample.Data.data()) / sample_size);
      }
      else if (mix_now < 0) {
         log.warning("Detected invalid mix values; TotalSamples: %d, MixNow: %d, SUE: %d, NextOffset: %d, Step: %d, ChannelPos: %d", TotalSamples, mix_now, sue, next_offset, step, Channel.Position);
         return;
      }

      // Check if we reached loop/sample/whatever end

      if (handle_sample_end(Self, Channel)) return;
   }
}

//********************************************************************************************************************
// Mono output filtering routines.

static void filter_float_mono(extAudio *Self, float *Data, int TotalSamples)
{
#ifdef AUDIO_WORKER
   auto &d1l = Self->FilterHistory[0];
   auto &d2l = Self->FilterHistory[1];
#else
   static float d1l=0, d2l=0;
#endif

   if ((Self->Flags & ADF::FILTER_LOW) != ADF::NIL) {
      while (TotalSamples > 0) {
         float s = (d1l + 2.0f * Data[0]) * (1.0f / 3.0f);
         d1l = Data[0];
         *(Data++) = s;
         TotalSamples--;
      }
   }
   else if ((Self->Flags & ADF::FILTER_HIGH) != ADF::NIL) {
      while (TotalSamples > 0) {
         float s = (d1l + 3.0f * d2l + 4.0f * Data[0]) * (1.0f / 8.0f);
         d1l = d2l;
         d2l = Data[0];
         *(Data++) = s;
         TotalSamples--;
      }
   }
}

//********************************************************************************************************************
// Stereo output filtering routines.

static void filter_float_stereo(extAudio *Self, float *Data, int TotalSamples)
{
#ifdef AUDIO_WORKER
   auto &d1l = Self->FilterHistory[0];
   auto &d2l = Self->FilterHistory[1];
   auto &d1r = Self->FilterHistory[2];
   auto &d2r = Self->FilterHistory[3];
#else
   static double d1l = 0, d1r = 0, d2l = 0, d2r = 0;
#endif

   if ((Self->Flags & ADF::FILTER_LOW) != ADF::NIL) {
      while (TotalSamples > 0) {
         float s = (d1l + 2.0 * Data[0]) * (1.0 / 3.0);
         d1l = Data[0];
         *(Data++) = s;

         s = (d1r + 2.0 * Data[0]) * (1.0 / 3.0);
         d1r = Data[0];
         *(Data++) = s;

         TotalSamples--;
      }
   }
   else if ((Self->Flags & ADF::FILTER_HIGH) != ADF::NIL) {
      while (TotalSamples > 0) {
         float s = (d1l + 3.0 * d2l + 4.0 * Data[0]) * (1.0 / 8.0);
         d1l = d2l;
         d2l = Data[0];
         *(Data++) = s;

         s = (d1r + 3.0 * d2r + 4.0 * Data[0]) * (1.0 / 8.0);
         d1r = d2r;
         d2r = Data[0];
         *(Data++) = s;

         TotalSamples--;
      }
   }
}
