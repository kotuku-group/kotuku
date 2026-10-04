#pragma once

// Modulated feedback delay for the AudioFlanger processor.
//
// Every channel has one delay line that stores the input plus the recirculated delayed signal.  A sine LFO sweeps the
// read distance upwards from a minimum delay D to D + A and back.  With phase phi, rate r and signed feedback g:
//
//   delay   = max(D + A * (1 - cos(phi)) / 2, 1 frame)
//   tap[c]  = ring_read(c, delay)
//   ring[c] = input[c] + g * tap[c]
//   out[c]  = (1 - mix) * input[c] + mix * tap[c]
//   phi     = wrap(phi + 2 pi r / fs)
//
// The phase starts at zero on reset, so the sweep starts at the minimum delay, and advances once per frame.  Both
// channels share the LFO.  Reads occur before the current frame is written, so the read distance is never less than
// one frame; at rates below 10 kHz this clamps the shortest delays.  Rate, depth, feedback and mix ramp linearly over
// 10 ms from their current values; the rate ramps as a phase increment, so the phase stays continuous.  A minimum
// delay edit crossfades between two read heads that share the LFO, and an edit during the crossfade replaces a single
// queued delay, which starts when the crossfade finishes.
//
// Stability: linear interpolation and the head crossfade are convex combinations of stored values and |g| is at most
// FLANGER_MAX_FEEDBACK, so the loop is contractive for every supported setting, including while the read distance and
// coefficients vary, and stored values never exceed FLANGER_HEADROOM times the largest input.
//
// History is cleared lazily: reads at distances beyond the number of frames written since the last reset or
// retirement return zero, so reset() is constant-time.

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

constexpr int FLANGER_MIN_RATE = 8000;
constexpr int FLANGER_MAX_RATE = 192000;
constexpr double FLANGER_MAX_REACH = 15;          // Milliseconds; the maximum delay plus the maximum depth
constexpr double FLANGER_MAX_FEEDBACK = 0.9;      // Matches the published feedback magnitude limit of 90%
constexpr double FLANGER_HEADROOM = 1.0 / (1.0 - FLANGER_MAX_FEEDBACK); // Largest stored level relative to the input
constexpr double FLANGER_RESIDUAL = 1e-6;         // Residual bound relative to the peak input (-120 dB)
constexpr double FLANGER_INPUT_LIMIT = 1e30;      // Input magnitudes are clamped here; 10x this still fits a float

struct FlangerSettings {
   double Rate = 0.25;    // Hertz
   double Delay = 1;      // Milliseconds
   double Depth = 2;      // Milliseconds
   double Feedback = 30;  // Percent, signed
   double Mix = 50;       // Percent
};

// DSP-ready values derived from FlangerSettings for one sample rate.  Deriving them performs no allocation.

struct FlangerTarget {
   int SampleRate = 0;
   double Increment = 0;  // LFO phase advance per frame, in radians
   double Delay = 0;      // Minimum delay in frames
   double Depth = 0;      // Sweep above the minimum delay, in frames
   double Feedback = 0;   // Signed linear gain per pass
   double Mix = 0;        // Wet proportion, 0 to 1
};

//********************************************************************************************************************
// Ring capacity in frames: the maximum reach of 15 ms, rounded up, plus interpolation support and one frame of
// margin.

constexpr int flanger_capacity(int Rate)
{
   return int((int64_t(Rate) * int64_t(FLANGER_MAX_REACH) + 999) / 1000) + 2;
}

//********************************************************************************************************************
// Conservative frames, after input ends, until every value written to the ring is at most FLANGER_RESIDUAL times the
// largest input, for a feedback magnitude of at most Feedback and read distances of at most Reach frames.
//
// Proof outline.  Without input, each write is g times a convex combination of values written 1 to K = Reach frames
// earlier.  Stored values start at most H = FLANGER_HEADROOM times the input, so by induction every write in frames
// [jK, (j + 1)K) after the input ends is at most g^(j + 1) H times the input.  P = ceil(ln(H / residual) / ln(1 / g))
// windows therefore suffice, and 1% is added to P for the rounding of float storage.  Feedback is an envelope: the
// result holds while the actual magnitude stays at or below it, however it varies.

inline uint64_t flanger_decay_bound(double Feedback, int Reach)
{
   const double g = std::min(std::abs(Feedback), FLANGER_MAX_FEEDBACK);
   if (g <= 0) return 0; // Writes are silent as soon as the input ends
   const double passes = std::ceil(std::log(FLANGER_HEADROOM / FLANGER_RESIDUAL) / -std::log(g) * 1.01);
   return uint64_t(std::max(1, Reach)) * uint64_t(passes);
}

//********************************************************************************************************************
// The first pass plus enough recirculations for a 60 dB decay, each at the longest read distance.

inline uint64_t flanger_decay_estimate(int Reach, double Feedback)
{
   const double g = std::abs(Feedback);
   double repeats = 0;
   if ((g > 0) and (g < 1)) repeats = std::ceil(std::log(0.001) / std::log(g));
   return uint64_t(double(Reach) * (1.0 + repeats));
}

//********************************************************************************************************************
// Derive the complete processing target.  Returns false for unsupported rates.

inline bool flanger_target(const FlangerSettings &Settings, int Rate, FlangerTarget &Target)
{
   if ((Rate < FLANGER_MIN_RATE) or (Rate > FLANGER_MAX_RATE)) return false;
   Target.SampleRate = Rate;
   Target.Increment  = 2.0 * std::numbers::pi * Settings.Rate / double(Rate);
   Target.Delay      = Settings.Delay * double(Rate) / 1000.0;
   Target.Depth      = Settings.Depth * double(Rate) / 1000.0;
   Target.Feedback   = std::clamp(Settings.Feedback / 100.0, -FLANGER_MAX_FEEDBACK, FLANGER_MAX_FEEDBACK);
   Target.Mix        = Settings.Mix / 100.0;
   return true;
}

//********************************************************************************************************************

class FlangerProcessor final : public AudioEffectProcessor {
public:
   extAudioEffect *Owner;
   FlangerSettings Settings;  // Committed parameters, read by reset() under the mixer lock
   FlangerTarget Target;      // Derived from Settings for the current rate while active
   int SampleRate = 0;
   int Channels = 1;

private:
   // Storage prepared for storage_rate and storage_channels, swapped in by Configuration::publish().
   std::vector<float> ring;  // capacity frames, interleaved
   int storage_rate = 0, storage_channels = 0, capacity = 0;

   int cursor = 0;           // Slot of the oldest frame, which is replaced by the next write
   int written = 0;          // Frames written since history was last cleared, up to capacity
   double phase = 0;         // LFO phase in radians, in [0, 2 pi)

   double delay = 0, old_delay = 0, pending_delay = 0; // Minimum delays of the read heads, in frames
   int delay_left = 0;       // Frames left in the read-head crossfade
   bool has_pending_delay = false;

   // Smoothed values: phase increment, depth in frames, signed feedback gain and wet mix.
   double increment = 0, depth = 0, gain = 0, mix = 0;
   double increment_end = 0, depth_end = 0, gain_end = 0, mix_end = 0;
   double increment_step = 0, depth_step = 0, gain_step = 0, mix_step = 0;
   int ramp_left = 0, ramp_frames = 1;

   double reference = 0;     // Peak input magnitude since history was last cleared
   int quiet = 0;            // Consecutive frames in which every written value was below the residual bound
   uint64_t tail_cache = 0, decay_cache = 0;
   bool active = false;
   bool live = false;        // History may hold non-zero values

   void start_delay(double Frames) {
      old_delay = delay;
      delay = Frames;
      delay_left = ramp_frames;
   }

   // A stored sample Distance frames before the frame being processed, for 1 <= Distance <= capacity.  Distances
   // beyond the frames written since the last clear read as silence.

   double sample(int Distance, int Channel) const {
      if (Distance > written) return 0;
      int index = cursor - Distance;
      if (index < 0) index += capacity;
      return ring[size_t(index) * Channels + Channel];
   }

   // Linear interpolation.  The weights are non-negative and sum to one, so the result never overshoots.  The lower
   // limit of one frame is part of the design; the upper limit is a defence that supported settings never reach.

   double read(double Distance, int Channel) const {
      Distance = std::clamp(Distance, 1.0, double(capacity - 1));
      const int whole = int(Distance);
      const double fraction = Distance - double(whole);
      const double near = sample(whole, Channel);
      if (fraction IS 0) return near;
      return near + (sample(whole + 1, Channel) - near) * fraction;
   }

   // Discard history once it is inaudible, so that a later mix edit cannot reveal it.  Constant time.

   void retire() {
      live = false;
      written = 0;
      reference = 0;
      quiet = 0;
   }

   // Bounds for the slowest decay that the current, ramp-target and queued values can produce.  Called on the
   // control thread under the mixer lock, or from reset().

   void refresh_bounds() {
      double longest = std::max(delay, Target.Delay);
      if (delay_left) longest = std::max(longest, old_delay);
      if (has_pending_delay) longest = std::max(longest, pending_delay);
      longest += std::max(depth, depth_end);
      const int reach = int(std::clamp(longest, 1.0, double(capacity - 1))) + 1;

      const double feedback = std::max(std::abs(gain), std::abs(gain_end));
      tail_cache = flanger_decay_bound(feedback, reach) + uint64_t(capacity) + 1;

      const uint64_t transitions = uint64_t(delay_left ? ramp_frames : 0) +
         uint64_t(has_pending_delay ? ramp_frames : 0);
      // The gain ramp can retain stronger feedback than its target.  Its maximum magnitude bounds every pass,
      // including a sign change, and keeps shared-path stop notifications behind the audible decay.
      decay_cache = flanger_decay_estimate(reach, feedback) + transitions;
   }

public:
   class Configuration final : public AudioEffectConfiguration {
   public:
      FlangerProcessor *Processor;
      std::vector<float> Ring;
      int Rate = 0, Channels = 0, Capacity = 0;

      explicit Configuration(FlangerProcessor *Target) : Processor(Target) { }

      // Swap under the mixer lock.  The processor's previous storage is retired here and freed after unlocking.
      void publish() override {
         auto &p = *Processor;
         p.ring.swap(Ring);
         p.storage_rate = Rate;
         p.storage_channels = Channels;
         p.capacity = Capacity;
         p.active = false;
         p.live = false;
         p.written = 0;
         p.cursor = 0;
      }

      int64_t latency() const override { return 0; }
   };

   FlangerProcessor(extAudioEffect *Effect, const FlangerSettings &Initial) : Owner(Effect), Settings(Initial) { }

   // Storage depends only on the rate and channel count and covers the maximum reach, so parameter changes never
   // allocate.  A zero rate leaves the processor inactive until the output is configured.

   ERR prepare(int PrepareRate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) override {
      auto config = std::make_unique<Configuration>(this);
      if (PrepareRate > 0) {
         if ((PrepareRate < FLANGER_MIN_RATE) or (PrepareRate > FLANGER_MAX_RATE)) return ERR::NoSupport;
         config->Rate = PrepareRate;
         config->Channels = Stereo ? 2 : 1;
         config->Capacity = flanger_capacity(PrepareRate);
         config->Ring.assign(size_t(config->Capacity) * config->Channels, 0.0f);
      }
      Result = std::move(config);
      return ERR::Okay;
   }

   // Caller holds the mixer lock.  Next is derived on the control thread for the rate that was current at the time,
   // or is null if that rate is unsupported.  Rate, depth, feedback and mix ramp over 10 ms from their current values.
   // A minimum delay change crossfades between two read heads over 10 ms; an edit during that crossfade replaces a
   // single queued delay, which starts when the current crossfade finishes.  History and phase are retained.

   void update(const FlangerSettings &NextSettings, const FlangerTarget *Next) {
      Settings = NextSettings;
      if ((not active) or Owner->ResetPending or (not Next) or (Next->SampleRate != SampleRate)) return;

      Target = *Next;
      increment_end = Next->Increment;
      depth_end = Next->Depth;
      gain_end = Next->Feedback;
      mix_end = Next->Mix;
      increment_step = (increment_end - increment) / double(ramp_frames);
      depth_step = (depth_end - depth) / double(ramp_frames);
      gain_step = (gain_end - gain) / double(ramp_frames);
      mix_step = (mix_end - mix) / double(ramp_frames);
      ramp_left = ramp_frames;

      if (delay_left) {
         pending_delay = Next->Delay;
         has_pending_delay = pending_delay != delay;
      }
      else {
         // A queued delay may still await the next frame after a crossfade ended at a block boundary.
         has_pending_delay = false;
         if (Next->Delay != delay) start_delay(Next->Delay);
      }

      refresh_bounds();
   }

   AudioTail tail() const override { return AudioTail::FINITE; }
   uint64_t tail_frames() const override { return active ? tail_cache : 0; }
   uint64_t decay_estimate() const override { return active ? decay_cache : 0; }
   bool pending() const override { return active and live; }

   double lfo_phase() const { return phase; }
   double lfo_increment() const { return increment; }
   double depth_frames() const { return depth; }
   double feedback_gain() const { return gain; }
   double wet_mix() const { return mix; }
   double min_delay() const { return delay; }
   bool crossfading() const { return delay_left > 0; }
   bool delay_queued() const { return has_pending_delay; }
   int ring_capacity() const { return capacity; }
   size_t ring_samples() const { return ring.size(); }
   const float *ring_storage() const { return ring.data(); }

   void reset() override {
      SampleRate = Owner->OutputRate;
      Channels = Owner->Stereo ? 2 : 1;
      retire();
      cursor = 0;
      phase = 0;
      delay_left = ramp_left = 0;
      has_pending_delay = false;

      active = (storage_rate > 0) and (SampleRate IS storage_rate) and (Channels <= storage_channels) and
         (Owner->Layout.size() <= 2) and flanger_target(Settings, SampleRate, Target);
      if (not active) return;

      ramp_frames = std::max(1, SampleRate / 100); // 10 ms
      delay = old_delay = pending_delay = Target.Delay;
      increment = increment_end = Target.Increment;
      depth = depth_end = Target.Depth;
      gain = gain_end = Target.Feedback;
      mix = mix_end = Target.Mix;
      increment_step = depth_step = gain_step = mix_step = 0;
      refresh_bounds();
   }

   // Non-finite input is outside the normalised pipeline contract and is treated as silence on entry.  Finite input
   // beyond FLANGER_INPUT_LIMIT is clamped so that the headroom of the feedback loop cannot overflow float storage.
   // While no history is live every readable sample is zero, so the reads are skipped.

   void process(float *Buffer, int Frames) override {
      if (not active) return;

      for (int frame = 0; frame < Frames; ++frame) {
         if ((not delay_left) and has_pending_delay) {
            start_delay(pending_delay);
            has_pending_delay = false;
         }

         float *io = Buffer + size_t(frame) * Channels;

         double dry[2], tap[2] = { 0, 0 };
         bool input_set = false;
         for (int c = 0; c < Channels; c++) {
            double x = std::isfinite(io[c]) ? double(io[c]) : 0.0;
            x = std::clamp(x, -FLANGER_INPUT_LIMIT, FLANGER_INPUT_LIMIT);
            dry[c] = x;
            input_set |= x != 0;
            reference = std::max(reference, std::abs(x));
         }

         if (live) {
            const double sweep = depth * 0.5 * (1.0 - std::cos(phase));
            const double head_blend = delay_left ? 1.0 - double(delay_left) / double(ramp_frames) : 1.0;
            for (int c = 0; c < Channels; c++) {
               double t = read(delay + sweep, c);
               if (delay_left) {
                  const double old_head = read(old_delay + sweep, c);
                  t = old_head + (t - old_head) * head_blend;
               }
               tap[c] = t;
            }
         }

         if (input_set) live = true;

         double peak = 0;
         if (live) {
            float *slot = ring.data() + size_t(cursor) * Channels;
            for (int c = 0; c < Channels; c++) {
               double value = dry[c] + gain * tap[c];
               const double magnitude = std::abs(value);
               if (magnitude < 1e-30) value = 0; // Keep float storage out of denormal range
               else peak = std::max(peak, magnitude);
               slot[c] = float(value);
            }
            if (written < capacity) written++;
         }
         if (++cursor IS capacity) cursor = 0;

         for (int c = 0; c < Channels; c++) io[c] = float((1.0 - mix) * dry[c] + mix * tap[c]);

         phase += increment;
         if (phase >= 2.0 * std::numbers::pi) phase -= 2.0 * std::numbers::pi;

         if (ramp_left) {
            if (--ramp_left) {
               increment += increment_step;
               depth += depth_step;
               gain += gain_step;
               mix += mix_step;
            }
            else {
               increment = increment_end;
               depth = depth_end;
               gain = gain_end;
               mix = mix_end;
            }
         }

         if (delay_left) --delay_left;

         if (live) {
            if (peak > std::max(reference * FLANGER_RESIDUAL, 1e-30)) quiet = 0;
            else if (++quiet >= capacity) retire();
         }
      }
   }
};
