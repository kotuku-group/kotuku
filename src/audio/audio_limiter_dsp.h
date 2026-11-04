#pragma once

// Stereo-linked lookahead sample-peak limiter for the AudioLimiter processor.
//
// Signal path, per frame: the input gain is applied and the result is written to a fixed delay of
// ceil(rate * 0.005) frames.  The linked detector peak of every frame, being the greatest absolute channel value, is
// held in a monotonic deque that yields the maximum over the window from the delayed frame through the newest
// input.  The gain reduction required to bring that maximum to the ceiling attacks immediately and releases
// exponentially in the dB domain, so attenuation is in place before a peak leaves the delay and is held until the
// peak has passed.  A final guard lowers the shared gain of the delayed frame if rounding or a live ceiling edit
// would otherwise leave any channel above the ceiling.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

constexpr int LIMITER_MIN_RATE = 8000;
constexpr int LIMITER_MAX_RATE = 192000;
constexpr double LIMITER_SNAP = 1e-12;                         // dB; releasing envelopes below this become zero
constexpr double LIMITER_DB_TO_LOG = std::numbers::ln10 / 20.0; // Natural-log gain per dB

struct LimiterSettings {
   double Ceiling = -1;   // dBFS
   double Release = 100;  // Milliseconds, exponential time constant
   double Gain = 0;       // dB, applied before detection
};

// DSP-ready values derived from LimiterSettings for one sample rate.

struct LimiterTarget {
   int Rate = 0;
   double Ceiling = 1;    // Linear magnitude, rounded down to the nearest float
   double Release = 0;    // One-pole coefficient
   double Gain = 1;       // Linear input gain
};

//********************************************************************************************************************
// The fixed lookahead: 5 ms rounded up to whole frames.

constexpr int limiter_lookahead(int Rate)
{
   return int((int64_t(Rate) * 5 + 999) / 1000);
}

//********************************************************************************************************************
// The largest float that does not exceed Value.  Output samples are floats, so a ceiling that is itself
// representable is never exceeded when a double at or below it is rounded to the nearest float.

inline double limiter_float_floor(double Value)
{
   float result = float(Value);
   if (double(result) > Value) result = std::nextafter(result, 0.0f);
   return result;
}

//********************************************************************************************************************
// Derive the complete processing target.  Returns false for unsupported rates.

inline bool limiter_target(const LimiterSettings &Settings, int Rate, LimiterTarget &Target)
{
   if ((Rate < LIMITER_MIN_RATE) or (Rate > LIMITER_MAX_RATE)) return false;
   Target.Rate    = Rate;
   Target.Ceiling = limiter_float_floor(std::pow(10.0, Settings.Ceiling / 20.0));
   Target.Release = std::exp(-1.0 / (Settings.Release * 0.001 * double(Rate)));
   Target.Gain    = std::pow(10.0, Settings.Gain / 20.0);
   return true;
}

//********************************************************************************************************************
// The gain reduction in dB that brings a non-negative Peak to a linear Ceiling.  Zero if no reduction is required.

inline double limiter_required(double Peak, double Ceiling)
{
   if (Peak <= Ceiling) return 0;
   return 20.0 * std::log10(Peak / Ceiling);
}

//********************************************************************************************************************

class LimiterProcessor final : public AudioEffectProcessor {
public:
   extAudioEffect *Owner;
   LimiterSettings Settings; // Committed parameters, read by reset() under the mixer lock
   LimiterTarget Target;     // Derived from Settings for the storage rate
   int Rate = 0;
   int Channels = 1;

   struct Peak {
      int64_t Sequence;
      double Level;
   };

private:
   // Storage prepared for storage_rate and storage_channels, swapped in by Configuration::publish().
   std::vector<double> delay;    // lookahead frames of pre-gained input, interleaved
   std::vector<Peak> peaks;      // Circular monotonic deque of lookahead + 1 entries
   int storage_rate = 0, storage_channels = 0, lookahead = 0;

   int cursor = 0;               // Delay slot holding the oldest frame, which is replaced by the next input
   int peak_head = 0, peak_count = 0;
   int occupied = 0;             // Delay frames holding a non-zero sample
   int64_t sequence = 0;         // Input frames since reset

   double gain = 1, gain_end = 1, gain_step = 0;
   double release = 0, release_end = 0, release_step = 0;
   int ramp_left = 0, ramp_frames = 1;
   double envelope_db = 0;       // Gain reduction, excluding the final guard
   double reduction = 0;         // Maximum applied reduction during the last process() call
   bool active = false;
   bool dirty = false;           // The delay may hold non-zero values

   void clear() {
      std::fill(delay.begin(), delay.end(), 0.0);
      dirty = false;
   }

   // Insert the newest linked peak and discard entries older than the delayed frame.  Smaller or equal peaks at the
   // back can never be the window maximum again, so the front always holds the maximum.

   double track(double Level) {
      const int capacity = int(peaks.size());
      while (peak_count and (peaks[peak_head].Sequence + lookahead < sequence)) {
         if (++peak_head IS capacity) peak_head = 0;
         peak_count--;
      }

      while (peak_count) {
         int back = peak_head + peak_count - 1;
         if (back >= capacity) back -= capacity;
         if (peaks[back].Level > Level) break;
         peak_count--;
      }

      int slot = peak_head + peak_count;
      if (slot >= capacity) slot -= capacity;
      peaks[slot] = { sequence, Level };
      peak_count++;
      return peaks[peak_head].Level;
   }

public:
   class Configuration final : public AudioEffectConfiguration {
   public:
      LimiterProcessor *Processor;
      std::vector<double> Delay;
      std::vector<Peak> Peaks;
      int Rate = 0, Channels = 0, Lookahead = 0;

      explicit Configuration(LimiterProcessor *Target) : Processor(Target) { }

      // Swap under the mixer lock.  The processor's previous storage is retired here and freed after unlocking.
      void publish() override {
         auto &p = *Processor;
         p.delay.swap(Delay);
         p.peaks.swap(Peaks);
         p.storage_rate = Rate;
         p.storage_channels = Channels;
         p.lookahead = Lookahead;
         p.active = false;
         p.dirty = false;
      }

      int64_t latency() const override { return Lookahead; }
   };

   LimiterProcessor(extAudioEffect *Effect, const LimiterSettings &Initial) : Owner(Effect), Settings(Initial) { }

   // Storage depends only on the rate and channel count, so parameter changes never allocate.  A zero rate leaves
   // the processor inactive, with no latency, until the output is configured.

   ERR prepare(int PrepareRate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) override {
      auto config = std::make_unique<Configuration>(this);
      if (PrepareRate > 0) {
         if ((PrepareRate < LIMITER_MIN_RATE) or (PrepareRate > LIMITER_MAX_RATE)) return ERR::NoSupport;
         config->Rate = PrepareRate;
         config->Channels = Stereo ? 2 : 1;
         config->Lookahead = limiter_lookahead(PrepareRate);
         config->Delay.assign(size_t(config->Lookahead) * config->Channels, 0.0);
         config->Peaks.resize(size_t(config->Lookahead) + 1);
      }
      Result = std::move(config);
      return ERR::Okay;
   }

   // Caller holds the mixer lock.  Next is derived on the control thread for the rate that was current at the time,
   // or is null if that rate is unsupported.  A lower or higher ceiling applies immediately; the input gain and
   // release coefficient ramp over 10 ms from their current values.

   void update(const LimiterSettings &NextSettings, const LimiterTarget *Next) {
      Settings = NextSettings;
      if (storage_rate <= 0) return;
      if (Next and (Next->Rate IS storage_rate)) Target = *Next;
      else limiter_target(Settings, storage_rate, Target);

      if ((not active) or Owner->ResetPending) return;

      gain_end = Target.Gain;
      release_end = Target.Release;
      gain_step = (gain_end - gain) / double(ramp_frames);
      release_step = (release_end - release) / double(ramp_frames);
      ramp_left = ramp_frames;
   }

   // Buffered input keeps the processor pending without a decaying tail.  The release envelope alone never creates
   // output from silence.

   AudioTail tail() const override { return AudioTail::NONE; }
   uint64_t tail_frames() const override { return uint64_t(lookahead); }
   int64_t latency() const override { return lookahead; }
   bool pending() const override { return active and (occupied > 0); }
   double gain_reduction() const override { return reduction; }
   double envelope() const { return envelope_db; }
   double input_gain() const { return gain; }
   double release_coefficient() const { return release; }
   size_t delay_samples() const { return delay.size(); }
   size_t peak_capacity() const { return peaks.size(); }
   const double *delay_storage() const { return delay.data(); }

   void reset() override {
      Rate = Owner->OutputRate;
      Channels = Owner->Stereo ? 2 : 1;
      if (dirty) clear();
      cursor = peak_head = peak_count = occupied = 0;
      sequence = 0;
      ramp_left = 0;
      envelope_db = 0;
      reduction = 0;

      active = (storage_rate > 0) and (Rate IS storage_rate) and (Channels <= storage_channels) and
         (Owner->Layout.size() <= 2) and limiter_target(Settings, Rate, Target);
      if (not active) return;

      ramp_frames = std::max(1, Rate / 100); // 10 ms
      gain = gain_end = Target.Gain;
      release = release_end = Target.Release;
      gain_step = release_step = 0;
   }

   // Non-finite input is outside the normalised pipeline contract.  Such samples are treated as silence on entry, so
   // they never reach the delay, the peak deque or the envelope.

   void process(float *Buffer, int Frames) override {
      reduction = 0;
      if (not active) return;
      dirty = true;

      const double ceiling = Target.Ceiling;
      for (int frame = 0; frame < Frames; ++frame) {
         float *io = Buffer + size_t(frame) * Channels;
         double *slot = delay.data() + size_t(cursor) * Channels;

         double delayed[2], level = 0, delayed_level = 0;
         bool input_set = false, delayed_set = false;
         for (int c = 0; c < Channels; c++) {
            const double sample = std::isfinite(io[c]) ? double(io[c]) * gain : 0.0;
            level = std::max(level, std::abs(sample));
            input_set |= sample != 0;

            delayed[c] = slot[c];
            delayed_level = std::max(delayed_level, std::abs(delayed[c]));
            delayed_set |= delayed[c] != 0;
            slot[c] = sample;
         }
         if (++cursor IS lookahead) cursor = 0;
         occupied += int(input_set) - int(delayed_set);

         const double window = track(level);
         ++sequence;

         const double required = limiter_required(window, ceiling);
         envelope_db = std::max(required, envelope_db * release);
         if (envelope_db < LIMITER_SNAP) envelope_db = 0;

         double applied = (envelope_db > 0) ? std::exp(-envelope_db * LIMITER_DB_TO_LOG) : 1.0;
         double applied_db = envelope_db;

         // The window includes the delayed frame and a lowered ceiling raises the required reduction immediately, so
         // the envelope already meets the ceiling and this guard only absorbs rounding in the exp() and log10()
         // conversions.  The ceiling is a float, so the rounded output of any value at or below it cannot exceed it.

         if (delayed_level * applied > ceiling) {
            applied = ceiling / delayed_level;
            applied_db = std::max(applied_db, -20.0 * std::log10(applied));
         }
         reduction = std::max(reduction, applied_db);

         for (int c = 0; c < Channels; c++) io[c] = float(delayed[c] * applied);

         if (ramp_left) {
            if (--ramp_left) {
               gain += gain_step;
               release += release_step;
            }
            else {
               gain = gain_end;
               release = release_end;
            }
         }
      }
   }
};
