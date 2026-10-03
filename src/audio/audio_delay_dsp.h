#pragma once

// Filtered feedback delay for the AudioDelay processor.
//
// Signal path, per frame and channel: the delayed signal d is read from a ring buffer by linear interpolation at a
// fractional distance, then passed through a one-pole low-pass s += a * (d - s).  The feedback signal is the blend
// fb = d + b * (s - d), where b fades the filter in across the lowest 1% of the damping range.  The ring receives the
// input plus g * fb, routed by an injection matrix I and a feedback matrix F:
//
//   normal:    I = [1 0; 0 1]          F = [1 0; 0 1]
//   ping-pong: I = [1/2 1/2; 0 0]      F = [0 1; 1 0]
//
// A mode change crossfades both matrices.  Every row of either endpoint has an absolute sum of at most one, and so
// does every convex combination of them, so the routing never amplifies in the maximum-channel norm.  The filter is
// a convex combination of past values and the interpolation weights are non-negative and sum to one.  With
// g <= DELAY_MAX_FEEDBACK the loop is therefore contractive for all supported parameters, including while
// coefficients ramp, and stored values never exceed DELAY_HEADROOM times the largest input.
//
// History is cleared lazily: reads at distances beyond the number of frames written since the last reset or
// retirement return zero, so reset() is constant-time at every rate.

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

constexpr int DELAY_MIN_RATE = 8000;
constexpr int DELAY_MAX_RATE = 192000;
constexpr double DELAY_MAX_TIME = 2000;          // Milliseconds; matches the published time maximum
constexpr double DELAY_MAX_FEEDBACK = 0.95;      // Matches the published feedback maximum of 95%
constexpr double DELAY_HEADROOM = 1.0 / (1.0 - DELAY_MAX_FEEDBACK); // Largest stored level relative to the input
constexpr double DELAY_RESIDUAL = 1e-6;          // Residual bound relative to the peak input (-120 dB)
constexpr double DELAY_INPUT_LIMIT = 1e30;       // Input magnitudes are clamped here; 20x this still fits a float
constexpr double DELAY_MIN_CORNER = 200;         // Hertz; damping corner at 100%
constexpr double DELAY_MAX_CORNER = 20000;       // Hertz; damping corner as damping approaches zero
constexpr double DELAY_MAX_CORNER_RATIO = 0.45;  // The damping corner never exceeds this fraction of the rate

enum { DELAY_NORMAL = 0, DELAY_PING_PONG = 1 };

struct DelaySettings {
   double Time = 250;      // Milliseconds
   double Feedback = 30;   // Percent
   double Damping = 25;    // Percent
   int Mode = DELAY_NORMAL;
   double Mix = 25;        // Percent
};

// DSP-ready values derived from DelaySettings for one sample rate.  Deriving them performs no allocation.

struct DelayTarget {
   int Rate = 0;
   double Delay = 0;           // Frames, fractional
   double Feedback = 0;        // Linear gain per repeat
   double Coefficient = 1;     // One-pole low-pass coefficient a, in (0, 1]
   double Blend = 0;           // Proportion of the low-pass output in the feedback path
   double Mix = 0;             // Wet proportion, 0 to 1
   int Mode = DELAY_NORMAL;
   uint64_t TailFrames = 0;    // Conservative bound for these values alone
   uint64_t DecayFrames = 0;   // Typical 60 dB decay for these values alone
};

//********************************************************************************************************************
// Ring capacity in frames: the maximum delay, plus one frame of interpolation support and one of margin.

constexpr int delay_capacity(int Rate)
{
   return int(int64_t(Rate) * int64_t(DELAY_MAX_TIME) / 1000) + 2;
}

//********************************************************************************************************************
// Damping maps the percentage p to a corner fc = Fmax * (200 / Fmax)^p, where Fmax = min(20 kHz, 0.45 fs), and then to
// the one-pole coefficient a = 1 - exp(-2 pi fc / fs).  Zero damping is exactly unfiltered.  The blend between the
// unfiltered and filtered signals rises linearly across the lowest 1% so that the response is continuous at zero.

inline void delay_damping(double Damping, int Rate, double &Coefficient, double &Blend)
{
   if (Damping <= 0) {
      Coefficient = 1;
      Blend = 0;
      return;
   }
   const double fmax = std::min(DELAY_MAX_CORNER, DELAY_MAX_CORNER_RATIO * double(Rate));
   const double corner = fmax * std::pow(DELAY_MIN_CORNER / fmax, Damping / 100.0);
   Coefficient = 1.0 - std::exp(-2.0 * std::numbers::pi * corner / double(Rate));
   Blend = std::min(1.0, Damping);
}

//********************************************************************************************************************
// Conservative frames, after input ends, until every stored value and filter state is below DELAY_RESIDUAL times the
// largest input, for a feedback gain of at most Feedback, a filter coefficient of at least Coefficient and read
// distances of at most Reach frames.
//
// Proof outline.  Without input, the ring receives w = g * fb with |fb| <= max(|d|, |s|), and d is a convex
// combination of ring values written 1 to Reach (K) frames earlier.  Suppose |fb(m)| <= C r^m and |s(m)| <= C r^m for
// all m < n, where history before time zero satisfies |w| <= g C.  Then |d(n)| <= g C r^(n-K), and
//
//   |s(n)| <= (1 - a) C r^(n-1) + a g C r^(n-K) <= C r^n   if   (1 - a) / r + a q <= 1, with q = g r^-K <= 1
//   |fb(n)| <= (1 - b) g C r^(n-K) + b C r^n <= C r^n      since q <= 1
//
// The left side of the first condition decreases with a, so the slowest filter governs, and a smaller g only
// shortens the decay.  The smallest valid r is the intersection of r = (g / q)^(1/K) and r = (1 - a) / (1 - a q) for
// q in [g, 1], found by bisection.  Stored values start at most DELAY_HEADROOM times the input, and covering the
// history requires C = DELAY_HEADROOM / g relative to the input.  The decay below the residual takes
// ln(C / residual) / -ln(r) frames, to which 1% and one frame are added for rounding.
//
// Feedback and Coefficient are envelopes: the result holds while the actual gain stays at or below Feedback and the
// actual coefficient at or above Coefficient, however they vary.  Because C grows as g falls, the result is not
// strictly monotonic in Feedback for very short delays and tiny gains, but each value remains a valid bound.

inline uint64_t delay_decay_bound(double Feedback, double Coefficient, int Reach)
{
   const double ratio = DELAY_HEADROOM / DELAY_RESIDUAL;
   const double a = std::clamp(Coefficient, 1e-12, 1.0);
   const int reach = std::max(1, Reach);

   if (Feedback <= 0) {
      // Writes are zero immediately; the filter state can follow history for one pass before decaying.
      if (a >= 1.0) return uint64_t(reach) + 1;
      return uint64_t(reach) + uint64_t(std::ceil(std::log(ratio) / -std::log1p(-a) * 1.01)) + 1;
   }

   const double g = std::min(Feedback, DELAY_MAX_FEEDBACK);
   const double inv_k = 1.0 / double(reach);
   auto head_rate = [&](double Q) { return std::exp((std::log(g) - std::log(Q)) * inv_k); };
   auto filter_rate = [&](double Q) { return (1.0 - a) / (1.0 - a * Q); };

   double low = g, high = 1.0;
   for (int i = 0; i < 64; i++) {
      const double mid = 0.5 * (low + high);
      if (head_rate(mid) > filter_rate(mid)) low = mid;
      else high = mid;
   }
   // The upper end of the bracket satisfies both conditions.
   const double rate = std::max(head_rate(high), filter_rate(high));
   const double log_rate = std::log(rate);
   if (not (log_rate < 0)) return UINT64_MAX;

   const double frames = std::log(ratio / g) / -log_rate * 1.01 + 1.0;
   if (not (frames < 1.8e19)) return UINT64_MAX;
   return uint64_t(std::ceil(frames));
}

//********************************************************************************************************************
// The integer read distance reached by a fractional delay, including interpolation support.

inline int delay_reach(double Delay)
{
   return int(Delay) + 1;
}

//********************************************************************************************************************
// The first echo plus enough repeats for an unfiltered 60 dB decay, then the 60 dB memory of the damping filter.

inline uint64_t delay_decay_estimate(double Delay, double Feedback, double Coefficient, double Blend)
{
   double repeats = 0;
   if ((Feedback > 0) and (Feedback < 1)) repeats = std::ceil(std::log(0.001) / std::log(Feedback));
   double frames = std::ceil(Delay) * (1.0 + repeats);
   if ((Blend > 0) and (Coefficient < 1)) frames += std::ceil(std::log(1000.0) / -std::log1p(-Coefficient));
   return uint64_t(frames);
}

//********************************************************************************************************************
// Derive the complete processing target.  Returns false for unsupported rates.

inline bool delay_target(const DelaySettings &Settings, int Rate, DelayTarget &Target)
{
   if ((Rate < DELAY_MIN_RATE) or (Rate > DELAY_MAX_RATE)) return false;
   Target.Rate     = Rate;
   Target.Delay    = Settings.Time * double(Rate) / 1000.0;
   Target.Feedback = Settings.Feedback / 100.0;
   Target.Mix      = Settings.Mix / 100.0;
   Target.Mode     = Settings.Mode;
   delay_damping(Settings.Damping, Rate, Target.Coefficient, Target.Blend);

   const uint64_t decay = delay_decay_bound(Target.Feedback, Target.Coefficient, delay_reach(Target.Delay));
   const uint64_t memory = uint64_t(delay_capacity(Rate));
   Target.TailFrames  = (decay > UINT64_MAX - memory) ? UINT64_MAX : decay + memory;
   Target.DecayFrames = delay_decay_estimate(Target.Delay, Target.Feedback, Target.Coefficient, Target.Blend);
   return true;
}

//********************************************************************************************************************

class DelayProcessor final : public AudioEffectProcessor {
public:
   extAudioEffect *Owner;
   DelaySettings Settings;   // Committed parameters, read by reset() under the mixer lock
   DelayTarget Target;       // Derived from Settings for the current rate while active
   int Rate = 0;
   int Channels = 1;

private:
   // Storage prepared for storage_rate and storage_channels, swapped in by Configuration::publish().
   std::vector<float> ring;  // capacity frames, interleaved
   int storage_rate = 0, storage_channels = 0, capacity = 0;

   int cursor = 0;           // Slot of the oldest frame, which is replaced by the next write
   int written = 0;          // Frames written since history was last cleared, up to capacity
   std::array<double, 2> state {};   // One-pole low-pass state per channel

   double delay = 0, old_delay = 0, pending_delay = 0; // Read distances in frames
   int delay_left = 0;       // Frames left in the read-head crossfade
   bool has_pending_delay = false;

   // Smoothed values: feedback gain, filter coefficient and blend, wet mix and the ping-pong routing weight.
   double gain = 0, coefficient = 1, blend = 0, mix = 0, routing = 0;
   double gain_end = 0, coefficient_end = 1, blend_end = 0, mix_end = 0, routing_end = 0;
   double gain_step = 0, coefficient_step = 0, blend_step = 0, mix_step = 0, routing_step = 0;
   int ramp_left = 0, ramp_frames = 1;

   double reference = 0;     // Peak input magnitude since history was last cleared
   int quiet = 0;            // Consecutive frames in which every stored value was below the residual bound
   uint64_t tail_cache = 0, decay_cache = 0;
   bool active = false;
   bool live = false;        // History may hold non-zero values

   void start_delay(double Frames) {
      old_delay = delay;
      delay = Frames;
      delay_left = ramp_frames;
   }

   // A stored sample Distance frames before the frame being processed.  Distances beyond the frames written since
   // the last clear read as silence.

   double sample(int Distance, int Channel) const {
      if (Distance > written) return 0;
      int index = cursor - Distance;
      if (index < 0) index += capacity;
      return ring[size_t(index) * Channels + Channel];
   }

   double read(double Distance, int Channel) const {
      const int whole = int(Distance);
      const double fraction = Distance - double(whole);
      const double near = sample(whole, Channel);
      if (fraction IS 0) return near;
      return near + (sample(whole + 1, Channel) - near) * fraction;
   }

   // Discard history once it is inaudible, so that a later mix or routing edit cannot reveal it.  Constant time.

   void retire() {
      live = false;
      written = 0;
      state = {};
      reference = 0;
      quiet = 0;
   }

   // Bounds for the slowest decay that the current, ramp-target and queued values can produce.  Called on the
   // control thread under the mixer lock, or from reset().

   void refresh_bounds() {
      const double feedback = std::max(gain, gain_end);
      const double filter = std::min(coefficient, coefficient_end);
      double longest = std::max(delay, Target.Delay);
      if (delay_left) longest = std::max(longest, old_delay);
      if (has_pending_delay) longest = std::max(longest, pending_delay);

      const uint64_t decay = delay_decay_bound(feedback, filter, delay_reach(longest));
      const uint64_t memory = uint64_t(capacity);
      tail_cache = (decay > UINT64_MAX - memory) ? UINT64_MAX : decay + memory;

      const uint64_t transitions = uint64_t(delay_left ? ramp_frames : 0) +
         uint64_t(has_pending_delay ? ramp_frames : 0);
      decay_cache = delay_decay_estimate(longest, Target.Feedback, Target.Coefficient, Target.Blend) + transitions;
   }

public:
   class Configuration final : public AudioEffectConfiguration {
   public:
      DelayProcessor *Processor;
      std::vector<float> Ring;
      int Rate = 0, Channels = 0, Capacity = 0;

      explicit Configuration(DelayProcessor *Target) : Processor(Target) { }

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

   DelayProcessor(extAudioEffect *Effect, const DelaySettings &Initial) : Owner(Effect), Settings(Initial) { }

   // Storage depends only on the rate and channel count and covers the maximum delay, so parameter changes never
   // allocate.  A zero rate leaves the processor inactive until the output is configured.

   ERR prepare(int PrepareRate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) override {
      auto config = std::make_unique<Configuration>(this);
      if (PrepareRate > 0) {
         if ((PrepareRate < DELAY_MIN_RATE) or (PrepareRate > DELAY_MAX_RATE)) return ERR::NoSupport;
         config->Rate = PrepareRate;
         config->Channels = Stereo ? 2 : 1;
         config->Capacity = delay_capacity(PrepareRate);
         config->Ring.assign(size_t(config->Capacity) * config->Channels, 0.0f);
      }
      Result = std::move(config);
      return ERR::Okay;
   }

   // Caller holds the mixer lock.  Next is derived on the control thread for the rate that was current at the time,
   // or is null if that rate is unsupported.  Feedback, damping, mix and routing ramp over 10 ms from their current
   // values.  A time change crossfades between two fixed read heads over 10 ms; an edit during that crossfade
   // replaces a single queued time, which starts when the current crossfade finishes.

   void update(const DelaySettings &NextSettings, const DelayTarget *Next) {
      Settings = NextSettings;
      if ((not active) or Owner->ResetPending or (not Next) or (Next->Rate != Rate)) return;

      Target = *Next;
      gain_end = Next->Feedback;
      coefficient_end = Next->Coefficient;
      blend_end = Next->Blend;
      mix_end = Next->Mix;
      routing_end = ((Channels IS 2) and (Next->Mode IS DELAY_PING_PONG)) ? 1.0 : 0.0;
      gain_step = (gain_end - gain) / double(ramp_frames);
      coefficient_step = (coefficient_end - coefficient) / double(ramp_frames);
      blend_step = (blend_end - blend) / double(ramp_frames);
      mix_step = (mix_end - mix) / double(ramp_frames);
      routing_step = (routing_end - routing) / double(ramp_frames);
      ramp_left = ramp_frames;

      if (delay_left) {
         pending_delay = Next->Delay;
         has_pending_delay = pending_delay != delay;
      }
      else if (Next->Delay != delay) start_delay(Next->Delay);

      refresh_bounds();
   }

   AudioTail tail() const override { return AudioTail::FINITE; }
   uint64_t tail_frames() const override { return active ? tail_cache : 0; }
   uint64_t decay_estimate() const override { return active ? decay_cache : 0; }
   bool pending() const override { return active and live; }

   double feedback_gain() const { return gain; }
   double filter_coefficient() const { return coefficient; }
   double filter_blend() const { return blend; }
   double wet_mix() const { return mix; }
   double routing_weight() const { return routing; }
   double read_distance() const { return delay; }
   bool crossfading() const { return delay_left > 0; }
   bool time_queued() const { return has_pending_delay; }
   int ring_capacity() const { return capacity; }
   size_t ring_samples() const { return ring.size(); }
   const float *ring_storage() const { return ring.data(); }

   void reset() override {
      Rate = Owner->OutputRate;
      Channels = Owner->Stereo ? 2 : 1;
      retire();
      cursor = 0;
      delay_left = ramp_left = 0;
      has_pending_delay = false;

      active = (storage_rate > 0) and (Rate IS storage_rate) and (Channels <= storage_channels) and
         (Owner->Layout.size() <= 2) and delay_target(Settings, Rate, Target);
      if (not active) return;

      ramp_frames = std::max(1, Rate / 100); // 10 ms
      delay = old_delay = pending_delay = Target.Delay;
      gain = gain_end = Target.Feedback;
      coefficient = coefficient_end = Target.Coefficient;
      blend = blend_end = Target.Blend;
      mix = mix_end = Target.Mix;
      routing = routing_end = ((Channels IS 2) and (Target.Mode IS DELAY_PING_PONG)) ? 1.0 : 0.0;
      gain_step = coefficient_step = blend_step = mix_step = routing_step = 0;
      tail_cache = Target.TailFrames;
      decay_cache = Target.DecayFrames;
   }

   // Non-finite input is outside the normalised pipeline contract and is treated as silence on entry.  Finite input
   // beyond DELAY_INPUT_LIMIT is clamped so that the headroom of the feedback loop cannot overflow float storage.

   void process(float *Buffer, int Frames) override {
      if (not active) return;

      for (int frame = 0; frame < Frames; ++frame) {
         if ((not delay_left) and has_pending_delay) {
            start_delay(pending_delay);
            has_pending_delay = false;
         }

         float *io = Buffer + size_t(frame) * Channels;
         const double head_blend = delay_left ? 1.0 - double(delay_left) / double(ramp_frames) : 1.0;

         double dry[2], delayed[2], feedback[2];
         bool input_set = false;
         for (int c = 0; c < Channels; c++) {
            double x = std::isfinite(io[c]) ? double(io[c]) : 0.0;
            x = std::clamp(x, -DELAY_INPUT_LIMIT, DELAY_INPUT_LIMIT);
            dry[c] = x;
            input_set |= x != 0;
            reference = std::max(reference, std::abs(x));

            double d = 0;
            if (live) {
               d = read(delay, c);
               if (delay_left) {
                  const double old_head = read(old_delay, c);
                  d = old_head + (d - old_head) * head_blend;
               }

               double &s = state[c];
               s += coefficient * (d - s);
               if (std::abs(s) < 1e-30) s = 0; // Keep the filter out of denormal range
            }
            delayed[c] = d;
            feedback[c] = d + blend * (state[c] - d);
         }

         if (input_set) live = true;

         double peak = 0;
         if (live) {
            double inject[2];
            if (Channels IS 1) inject[0] = dry[0] + gain * feedback[0];
            else {
               const double w = routing, keep = 1.0 - routing;
               inject[0] = keep * dry[0] + w * 0.5 * (dry[0] + dry[1]) +
                  gain * (keep * feedback[0] + w * feedback[1]);
               inject[1] = keep * dry[1] + gain * (keep * feedback[1] + w * feedback[0]);
            }

            float *slot = ring.data() + size_t(cursor) * Channels;
            for (int c = 0; c < Channels; c++) {
               double value = inject[c];
               const double magnitude = std::abs(value);
               if (magnitude < 1e-30) value = 0; // Keep float storage out of denormal range
               else peak = std::max(peak, magnitude);
               peak = std::max(peak, std::abs(state[c]));
               slot[c] = float(value);
            }
            if (written < capacity) written++;
         }
         if (++cursor IS capacity) cursor = 0;

         for (int c = 0; c < Channels; c++) io[c] = float((1.0 - mix) * dry[c] + mix * delayed[c]);

         if (ramp_left) {
            if (--ramp_left) {
               gain += gain_step;
               coefficient += coefficient_step;
               blend += blend_step;
               mix += mix_step;
               routing += routing_step;
            }
            else {
               gain = gain_end;
               coefficient = coefficient_end;
               blend = blend_end;
               mix = mix_end;
               routing = routing_end;
            }
         }

         if (delay_left) --delay_left;

         if (live) {
            if (peak > std::max(reference * DELAY_RESIDUAL, 1e-30)) quiet = 0;
            else if (++quiet >= capacity) retire();
         }
      }
   }
};
