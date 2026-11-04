#pragma once

// Modulated delay for the AudioChorus processor.
//
// Every channel has one delay line, read by linear interpolation at a distance that a sine LFO sweeps around a
// centre delay.  With phase phi, centre D and depth A in frames and a channel offset of zero on the left and
// pi * spread / 100 on the right:
//
//   delay[c] = D + A * sin(phi + offset[c])
//   out[c]   = (1 - mix) * input[c] + mix * ring_read(c, delay[c])
//   phi      = wrap(phi + 2 pi rate / fs)
//
// The phase starts at zero on reset and advances once per frame.  Rate, depth, spread and mix ramp linearly over
// 10 ms from their current values; the rate ramps as a phase increment, so the phase stays continuous.  A centre
// delay edit crossfades between two read heads that share the LFO, so the edit does not sweep the delay.  An edit
// during the crossfade replaces a single queued centre, which starts when the crossfade finishes.
//
// There is no feedback: the wet signal is a convex combination of stored input and the output is a convex
// combination of the dry and wet signals, so the chorus never adds gain and its history expires a fixed number of
// frames after the last non-zero input.

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

constexpr int CHORUS_MIN_RATE = 8000;
constexpr int CHORUS_MAX_RATE = 192000;
constexpr double CHORUS_MAX_REACH = 39;       // Milliseconds; the maximum centre plus the maximum depth
constexpr double CHORUS_DENORMAL = 1e-30;     // Stored magnitudes below this are flushed to zero

struct ChorusSettings {
   double Rate = 0.8;     // Hertz
   double Delay = 15;     // Milliseconds
   double Depth = 3;      // Milliseconds
   double Spread = 50;    // Percent
   double Mix = 35;       // Percent
};

// DSP-ready values derived from ChorusSettings for one sample rate.  Deriving them performs no allocation.

struct ChorusTarget {
   int SampleRate = 0;
   double Increment = 0;  // LFO phase advance per frame, in radians
   double Delay = 0;      // Centre delay in frames
   double Depth = 0;      // Peak excursion in frames
   double Offset = 0;     // Right-channel phase offset in radians
   double Mix = 0;        // Wet proportion, 0 to 1
};

//********************************************************************************************************************
// Ring capacity in frames: the maximum reach of 39 ms, rounded up, plus interpolation support and one frame of
// margin.  This is also the number of frames for which history remains readable after the last non-zero input.

constexpr int chorus_capacity(int Rate)
{
   return int((int64_t(Rate) * int64_t(CHORUS_MAX_REACH) + 999) / 1000) + 2;
}

//********************************************************************************************************************
// Derive the complete processing target.  Returns false for unsupported rates.

inline bool chorus_target(const ChorusSettings &Settings, int Rate, ChorusTarget &Target)
{
   if ((Rate < CHORUS_MIN_RATE) or (Rate > CHORUS_MAX_RATE)) return false;
   Target.SampleRate = Rate;
   Target.Increment  = 2.0 * std::numbers::pi * Settings.Rate / double(Rate);
   Target.Delay      = Settings.Delay * double(Rate) / 1000.0;
   Target.Depth      = Settings.Depth * double(Rate) / 1000.0;
   Target.Offset     = std::numbers::pi * Settings.Spread / 100.0;
   Target.Mix        = Settings.Mix / 100.0;
   return true;
}

//********************************************************************************************************************

class ChorusProcessor final : public AudioEffectProcessor {
public:
   extAudioEffect *Owner;
   ChorusSettings Settings;  // Committed parameters, read by reset() under the mixer lock
   ChorusTarget Target;      // Derived from Settings for the current rate while active
   int SampleRate = 0;
   int Channels = 1;

private:
   // Storage prepared for storage_rate and storage_channels, swapped in by Configuration::publish().
   std::vector<float> ring;  // capacity frames, interleaved
   int storage_rate = 0, storage_channels = 0, capacity = 0;

   int cursor = 0;           // Slot of the oldest frame, which is replaced by the next write
   int countdown = 0;        // Frames for which non-zero history may remain readable
   double phase = 0;         // LFO phase in radians, in [0, 2 pi)

   double centre = 0, old_centre = 0, pending_centre = 0; // Centre delays of the read heads, in frames
   int centre_left = 0;      // Frames left in the read-head crossfade
   bool has_pending_centre = false;

   // Smoothed values: phase increment, depth in frames, right-channel phase offset and wet mix.
   double increment = 0, depth = 0, offset = 0, mix = 0;
   double increment_end = 0, depth_end = 0, offset_end = 0, mix_end = 0;
   double increment_step = 0, depth_step = 0, offset_step = 0, mix_step = 0;
   int ramp_left = 0, ramp_frames = 1;
   bool active = false;

   void start_centre(double Frames) {
      old_centre = centre;
      centre = Frames;
      centre_left = ramp_frames;
   }

   // A stored sample Distance frames before the frame being processed, for 1 <= Distance <= capacity.

   double sample(int Distance, int Channel) const {
      int index = cursor - Distance;
      if (index < 0) index += capacity;
      return ring[size_t(index) * Channels + Channel];
   }

   // Linear interpolation.  The weights are non-negative and sum to one, so the result never overshoots.  The
   // distance is held inside the readable range as a defence; supported settings never reach either limit.

   double read(double Distance, int Channel) const {
      Distance = std::clamp(Distance, 1.0, double(capacity - 1));
      const int whole = int(Distance);
      const double fraction = Distance - double(whole);
      const double near = sample(whole, Channel);
      if (fraction IS 0) return near;
      return near + (sample(whole + 1, Channel) - near) * fraction;
   }

public:
   class Configuration final : public AudioEffectConfiguration {
   public:
      ChorusProcessor *Processor;
      std::vector<float> Ring;
      int Rate = 0, Channels = 0, Capacity = 0;

      explicit Configuration(ChorusProcessor *Target) : Processor(Target) { }

      // Swap under the mixer lock.  The processor's previous storage is retired here and freed after unlocking.
      void publish() override {
         auto &p = *Processor;
         p.ring.swap(Ring);
         p.storage_rate = Rate;
         p.storage_channels = Channels;
         p.capacity = Capacity;
         p.active = false;
         p.countdown = 0;
         p.cursor = 0;
      }

      int64_t latency() const override { return 0; }
   };

   ChorusProcessor(extAudioEffect *Effect, const ChorusSettings &Initial) : Owner(Effect), Settings(Initial) { }

   // Storage depends only on the rate and channel count and covers the maximum reach, so parameter changes never
   // allocate.  A zero rate leaves the processor inactive until the output is configured.

   ERR prepare(int PrepareRate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) override {
      auto config = std::make_unique<Configuration>(this);
      if (PrepareRate > 0) {
         if ((PrepareRate < CHORUS_MIN_RATE) or (PrepareRate > CHORUS_MAX_RATE)) return ERR::NoSupport;
         config->Rate = PrepareRate;
         config->Channels = Stereo ? 2 : 1;
         config->Capacity = chorus_capacity(PrepareRate);
         config->Ring.assign(size_t(config->Capacity) * config->Channels, 0.0f);
      }
      Result = std::move(config);
      return ERR::Okay;
   }

   // Caller holds the mixer lock.  Next is derived on the control thread for the rate that was current at the time,
   // or is null if that rate is unsupported.  Rate, depth, spread and mix ramp over 10 ms from their current values.
   // A centre change crossfades between two read heads over 10 ms; an edit during that crossfade replaces a single
   // queued centre, which starts when the current crossfade finishes.  History and phase are retained.

   void update(const ChorusSettings &NextSettings, const ChorusTarget *Next) {
      Settings = NextSettings;
      if ((not active) or Owner->ResetPending or (not Next) or (Next->SampleRate != SampleRate)) return;

      Target = *Next;
      increment_end = Next->Increment;
      depth_end = Next->Depth;
      offset_end = Next->Offset;
      mix_end = Next->Mix;
      increment_step = (increment_end - increment) / double(ramp_frames);
      depth_step = (depth_end - depth) / double(ramp_frames);
      offset_step = (offset_end - offset) / double(ramp_frames);
      mix_step = (mix_end - mix) / double(ramp_frames);
      ramp_left = ramp_frames;

      if (centre_left) {
         pending_centre = Next->Delay;
         has_pending_centre = pending_centre != centre;
      }
      else {
         // A queued centre may still await the next frame after a crossfade ended at a block boundary.
         has_pending_centre = false;
         if (Next->Delay != centre) start_centre(Next->Delay);
      }
   }

   // The tail is the readable history alone, independent of the settings, so that no later edit can reveal a sample
   // after the processor has stopped reporting it.

   AudioTail tail() const override { return AudioTail::FINITE; }
   uint64_t tail_frames() const override { return active ? uint64_t(capacity) : 0; }
   uint64_t decay_estimate() const override { return active ? uint64_t(capacity) : 0; }
   bool pending() const override { return active and (countdown > 0); }

   double lfo_phase() const { return phase; }
   double lfo_increment() const { return increment; }
   double depth_frames() const { return depth; }
   double phase_offset() const { return offset; }
   double wet_mix() const { return mix; }
   double centre_delay() const { return centre; }
   bool crossfading() const { return centre_left > 0; }
   bool centre_queued() const { return has_pending_centre; }
   int ring_capacity() const { return capacity; }
   size_t ring_samples() const { return ring.size(); }
   const float *ring_storage() const { return ring.data(); }

   void reset() override {
      SampleRate = Owner->OutputRate;
      Channels = Owner->Stereo ? 2 : 1;
      std::fill(ring.begin(), ring.end(), 0.0f);
      cursor = countdown = 0;
      phase = 0;
      centre_left = ramp_left = 0;
      has_pending_centre = false;

      active = (storage_rate > 0) and (SampleRate IS storage_rate) and (Channels <= storage_channels) and
         (Owner->Layout.size() <= 2) and chorus_target(Settings, SampleRate, Target);
      if (not active) return;

      ramp_frames = std::max(1, SampleRate / 100); // 10 ms
      centre = old_centre = pending_centre = Target.Delay;
      increment = increment_end = Target.Increment;
      depth = depth_end = Target.Depth;
      offset = offset_end = Target.Offset;
      mix = mix_end = Target.Mix;
      increment_step = depth_step = offset_step = mix_step = 0;
   }

   // Non-finite input is outside the normalised pipeline contract and is treated as silence on entry.  Reads occur
   // before the current frame is written, so the shortest distance of one frame is the previous input.  While the
   // countdown is zero every readable sample is zero, so the wet signal is silent and the reads are skipped.

   void process(float *Buffer, int Frames) override {
      if (not active) return;

      for (int frame = 0; frame < Frames; ++frame) {
         if ((not centre_left) and has_pending_centre) {
            start_centre(pending_centre);
            has_pending_centre = false;
         }

         float *io = Buffer + size_t(frame) * Channels;
         float *slot = ring.data() + size_t(cursor) * Channels;
         const bool history = countdown > 0;
         const double head_blend = centre_left ? 1.0 - double(centre_left) / double(ramp_frames) : 1.0;

         bool input_set = false;
         for (int c = 0; c < Channels; c++) {
            const double x = std::isfinite(io[c]) ? double(io[c]) : 0.0;

            double wet = 0;
            if (history) {
               const double sweep = depth * std::sin(c ? phase + offset : phase);
               wet = read(centre + sweep, c);
               if (centre_left) {
                  const double old_head = read(old_centre + sweep, c);
                  wet = old_head + (wet - old_head) * head_blend;
               }
            }

            if (std::abs(x) < CHORUS_DENORMAL) slot[c] = 0.0f; // Keep stored history out of denormal range
            else {
               slot[c] = float(x);
               input_set = true;
            }

            io[c] = float((1.0 - mix) * x + mix * wet);
         }

         if (++cursor IS capacity) cursor = 0;

         if (input_set) countdown = capacity;
         else if (countdown > 0) countdown--;

         phase += increment;
         if (phase >= 2.0 * std::numbers::pi) phase -= 2.0 * std::numbers::pi;

         if (ramp_left) {
            if (--ramp_left) {
               increment += increment_step;
               depth += depth_step;
               offset += offset_step;
               mix += mix_step;
            }
            else {
               increment = increment_end;
               depth = depth_end;
               offset = offset_end;
               mix = mix_end;
            }
         }

         if (centre_left) --centre_left;
      }
   }
};
