#pragma once

// Headphone crossfeed for the AudioCrossfeed processor.
//
// For stereo input L, R with crossfeed amount a, trim t, low-pass LP and a fractional delay of D frames:
//
//   L' = t * (L + a * LP(R)[n - D]) / (1 + a)
//   R' = t * (R + a * LP(L)[n - D]) / (1 + a)
//
// The direct path is immediate, so the effect has no latency.  Mono output is scaled by t only.
//
// Filter.  LP is a first-order bilinear low-pass with its -3 dB point prewarped to the cutoff, written in terms of its
// pole p = (1 - K) / (1 + K), K = tan(pi fc / fs):
//
//   y[n] = (1 - p) / 2 * (x[n] + x[n - 1]) + p * y[n - 1]
//
// The DC gain is exactly one for any p.  For supported cutoffs and rates fc <= fs / 4, so K <= 1 and 0 <= p < 1: every
// output is a convex combination of the inputs and the previous output, and remains so while p ramps.  The delay is
// read from a ring of filtered values by linear interpolation, whose weights are also non-negative and sum to one.
// The crossfed term is therefore never larger than the peak input, and before trim neither output can exceed the peak
// input.  The corner is limited to CROSSFEED_MAX_CORNER_RATIO of the rate, which does not engage for any published
// cutoff at a supported rate.
//
// The amount, pole and trim ramp linearly over 10 ms from their current values; the normalisation 1 / (1 + a) follows
// the amount.  A delay change crossfades between two read heads over 10 ms; an edit during the crossfade replaces a
// single queued delay, which starts when the crossfade ends.  The filters run on every stereo frame regardless of the
// amount, so raising it from zero has no start-up transient.
//
// Tail.  When the input stops, the filter output decays as p^n from at most the peak input, and the ring then holds at
// most CROSSFEED_CAPACITY frames of it.  History is discarded once the input is silent and every value that a read
// head can reach is below CROSSFEED_RESIDUAL times the peak input, which also stops denormal values.

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

constexpr int CROSSFEED_MIN_RATE = 8000;
constexpr int CROSSFEED_MAX_RATE = 192000;
constexpr double CROSSFEED_MAX_DELAY = 1.0;          // Milliseconds; matches the published delay maximum
constexpr double CROSSFEED_MAX_CORNER_RATIO = 0.45;  // The low-pass corner never exceeds this fraction of the rate
constexpr double CROSSFEED_RESIDUAL = 1e-7;          // History is discarded below this proportion of the peak input
constexpr int CROSSFEED_CAPACITY = 256;              // Ring frames; a power of two above the longest reach

static_assert(int(CROSSFEED_MAX_RATE * CROSSFEED_MAX_DELAY / 1000) + 2 <= CROSSFEED_CAPACITY);

struct CrossfeedSettings {
   double Amount = 20;     // Percent
   double Cutoff = 700;    // Hertz
   double Delay = 0.3;     // Milliseconds
   double Gain = 0;        // Decibels
};

// DSP-ready values derived from CrossfeedSettings for one sample rate.  Deriving them performs no allocation.

struct CrossfeedTarget {
   int SampleRate = 0;
   double Amount = 0;      // Linear crossfeed proportion a
   double Pole = 0;        // Low-pass pole p
   double Corner = 0;      // Effective low-pass corner in Hertz
   double Delay = 0;       // Frames, fractional
   double Trim = 1;        // Linear output gain
};

//********************************************************************************************************************
// The corner is limited below the Nyquist frequency so that the prewarped pole stays inside the unit circle.

inline double crossfeed_corner(double Cutoff, int Rate)
{
   return std::min(Cutoff, double(Rate) * CROSSFEED_MAX_CORNER_RATIO);
}

inline double crossfeed_pole(double Cutoff, int Rate)
{
   const double k = std::tan(std::numbers::pi * crossfeed_corner(Cutoff, Rate) / double(Rate));
   return (1.0 - k) / (1.0 + k);
}

// The longest distance, in frames, that a read head can reach at the rate, including interpolation support.

inline int crossfeed_reach(int Rate)
{
   return int(double(Rate) * CROSSFEED_MAX_DELAY / 1000.0) + 1;
}

// Frames after the input stops until a filter with pole p has fallen below CROSSFEED_RESIDUAL times the peak input.
// The first silent frame still includes the last input, after which the output decays by p per frame.

inline uint64_t crossfeed_filter_tail(double Pole)
{
   if (Pole <= 1e-300) return 2;
   return uint64_t(std::ceil(std::log(CROSSFEED_RESIDUAL) / std::log(Pole))) + 2;
}

// Typical frames for the crossfed signal to decay by 60 dB: the delay plus the 60 dB time of the pole.

inline uint64_t crossfeed_decay_estimate(double Pole, double Delay)
{
   uint64_t frames = uint64_t(std::ceil(Delay)) + 1;
   if (Pole > 1e-300) frames += uint64_t(std::ceil(std::log(0.001) / std::log(Pole)));
   return frames;
}

//********************************************************************************************************************
// Derive the complete processing target.  Returns false for unsupported rates.

inline bool crossfeed_target(const CrossfeedSettings &Settings, int Rate, CrossfeedTarget &Target)
{
   if ((Rate < CROSSFEED_MIN_RATE) or (Rate > CROSSFEED_MAX_RATE)) return false;
   Target.SampleRate = Rate;
   Target.Amount     = Settings.Amount / 100.0;
   Target.Corner     = crossfeed_corner(Settings.Cutoff, Rate);
   Target.Pole       = crossfeed_pole(Settings.Cutoff, Rate);
   Target.Delay      = std::clamp(Settings.Delay, 0.0, CROSSFEED_MAX_DELAY) * double(Rate) / 1000.0;
   Target.Trim       = std::pow(10.0, Settings.Gain / 20.0);
   return true;
}

//********************************************************************************************************************

class CrossfeedProcessor final : public AudioEffectProcessor {
public:
   // A value that ramps linearly to End over the remaining frames of the shared ramp.

   struct Ramp {
      double Value = 0, End = 0, Step = 0;

      void snap(double Target) { Value = End = Target; Step = 0; }
      void start(double Target, int Frames) { End = Target; Step = (End - Value) / double(Frames); }
      void advance(bool Last) { Value = Last ? End : Value + Step; }
   };

   extAudioEffect *Owner;
   CrossfeedSettings Settings; // Committed parameters, read by reset() under the mixer lock
   CrossfeedTarget Target;     // Derived from Settings for the current rate while active
   int SampleRate = 0;
   int Channels = 1;

private:
   int configured_rate = 0;    // Rate validated by prepare() and published under the mixer lock

   Ramp amount, pole, trim;
   int ramp_left = 0, ramp_frames = 1;

   // Filtered values per channel, interleaved.  cursor is the slot of the most recent frame.
   std::array<double, size_t(CROSSFEED_CAPACITY) * 2> ring {};
   int cursor = 0;
   int written = 0;            // Frames written since history was last cleared, up to CROSSFEED_CAPACITY
   std::array<double, 2> x1 {}, y1 {}; // Filter state per channel

   double delay = 0, old_delay = 0, pending_delay = 0; // Read distances in frames
   int delay_left = 0;         // Frames left in the read-head crossfade
   bool has_pending_delay = false;

   double reference = 0;       // Peak input magnitude since history was last cleared
   int quiet = 0;              // Consecutive silent frames whose filter output was below the residual
   int reach = 1;              // Longest read distance at the current rate
   uint64_t tail_cache = 0, decay_cache = 0;
   bool active = false;
   bool live = false;          // History may hold non-zero values

   void retire() {
      live = false;
      written = 0;
      x1 = {};
      y1 = {};
      reference = 0;
      quiet = 0;
   }

   void start_delay(double Frames) {
      old_delay = delay;
      delay = Frames;
      delay_left = ramp_frames;
   }

   // A filtered value Distance frames before the current frame.  Distances beyond the frames written since the last
   // clear read as silence.

   double sample(int Distance, int Channel) const {
      if (Distance >= written) return 0;
      return ring[size_t((cursor - Distance) & (CROSSFEED_CAPACITY - 1)) * 2 + Channel];
   }

   double read(double Distance, int Channel) const {
      const int whole = int(Distance);
      const double fraction = Distance - double(whole);
      const double near = sample(whole, Channel);
      if (fraction IS 0) return near;
      return near + (sample(whole + 1, Channel) - near) * fraction;
   }

   // The crossfed signal is audible while the amount is or will become non-zero.

   bool audible() const { return (amount.Value != 0) or (amount.End != 0); }

   // Called under the mixer lock, or from reset().  The bounds cover the slowest of the current and target poles, the
   // ring reach and any remaining delay crossfades and ramps.

   void refresh_bounds() {
      const double slowest = std::max(pole.Value, pole.End);
      const uint64_t transitions = uint64_t(delay_left) + (has_pending_delay ? uint64_t(ramp_frames) : 0);
      double longest = delay;
      if (delay_left) longest = std::max(longest, old_delay);
      if (has_pending_delay) longest = std::max(longest, pending_delay);
      tail_cache = crossfeed_filter_tail(slowest) + uint64_t(reach) + 1 + transitions + uint64_t(ramp_left);
      decay_cache = crossfeed_decay_estimate(slowest, longest) + transitions;
   }

public:
   class Configuration final : public AudioEffectConfiguration {
   public:
      CrossfeedProcessor *Processor;
      int Rate = 0;

      Configuration(CrossfeedProcessor *Target, int PreparedRate) : Processor(Target), Rate(PreparedRate) { }

      void publish() override {
         auto &p = *Processor;
         p.configured_rate = Rate;
         p.active = false;
         p.retire();
      }

      int64_t latency() const override { return 0; }
   };

   CrossfeedProcessor(extAudioEffect *Effect, const CrossfeedSettings &Initial) : Owner(Effect), Settings(Initial) { }

   // The ring is a fixed member that covers the longest delay at every supported rate, so preparation only validates
   // the rate.  A zero rate leaves the processor inactive until the output is configured.

   ERR prepare(int PrepareRate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) override {
      if ((PrepareRate > 0) and ((PrepareRate < CROSSFEED_MIN_RATE) or (PrepareRate > CROSSFEED_MAX_RATE))) {
         return ERR::NoSupport;
      }
      Result = std::make_unique<Configuration>(this, PrepareRate);
      return ERR::Okay;
   }

   // Caller holds the mixer lock.  Next is derived on the control thread for the rate that was current at the time,
   // or is null if that rate is unsupported.  The amount, pole and trim ramp over 10 ms from their current values.  A
   // delay change crossfades between two read heads; an edit during that crossfade replaces a single queued delay.
   // History is retained.

   void update(const CrossfeedSettings &NextSettings, const CrossfeedTarget *Next) {
      Settings = NextSettings;
      if ((not active) or Owner->ResetPending or (not Next) or (Next->SampleRate != SampleRate)) return;

      Target = *Next;
      amount.start(Next->Amount, ramp_frames);
      pole.start(Next->Pole, ramp_frames);
      trim.start(Next->Trim, ramp_frames);
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
   uint64_t tail_frames() const override { return (active and (Channels IS 2) and audible()) ? tail_cache : 0; }
   uint64_t decay_estimate() const override {
      return (active and (Channels IS 2) and audible()) ? decay_cache : 0;
   }
   bool pending() const override { return active and (Channels IS 2) and live and audible(); }

   double amount_gain() const { return amount.Value; }
   double filter_pole() const { return pole.Value; }
   double trim_gain() const { return trim.Value; }
   double read_distance() const { return delay; }
   bool crossfading() const { return delay_left > 0; }
   bool delay_queued() const { return has_pending_delay; }
   bool history_live() const { return live; }

   void reset() override {
      SampleRate = Owner->OutputRate;
      Channels = Owner->Stereo ? 2 : 1;
      retire();
      cursor = 0;
      ramp_left = delay_left = 0;
      has_pending_delay = false;

      active = (configured_rate > 0) and (SampleRate IS configured_rate) and (Owner->Layout.size() <= 2) and
         crossfeed_target(Settings, SampleRate, Target);
      if (not active) return;

      ramp_frames = std::max(1, SampleRate / 100); // 10 ms
      reach = crossfeed_reach(SampleRate);
      amount.snap(Target.Amount);
      pole.snap(Target.Pole);
      trim.snap(Target.Trim);
      delay = old_delay = pending_delay = Target.Delay;
      refresh_bounds();
   }

   // Non-finite input is outside the normalised pipeline contract and is treated as silence.

   void process(float *Buffer, int Frames) override {
      if (not active) return;

      if (Channels IS 1) {
         for (int frame = 0; frame < Frames; ++frame) {
            const double x = std::isfinite(Buffer[frame]) ? double(Buffer[frame]) : 0.0;
            Buffer[frame] = float(x * trim.Value);
            if (ramp_left) {
               --ramp_left;
               amount.advance(not ramp_left); pole.advance(not ramp_left); trim.advance(not ramp_left);
            }
         }
         return;
      }

      for (int frame = 0; frame < Frames; ++frame) {
         if ((not delay_left) and has_pending_delay) {
            start_delay(pending_delay);
            has_pending_delay = false;
         }

         float *io = Buffer + size_t(frame) * 2;
         const double l = std::isfinite(io[0]) ? double(io[0]) : 0.0;
         const double r = std::isfinite(io[1]) ? double(io[1]) : 0.0;
         const bool silent = (l IS 0) and (r IS 0);

         if (not silent) {
            live = true;
            reference = std::max(reference, std::max(std::abs(l), std::abs(r)));
         }

         // into_left is the crossfed signal from the right channel, and into_right from the left.

         double into_left = 0, into_right = 0;
         if (live) {
            const double p = pole.Value, b = (1.0 - p) * 0.5;
            const double fl = b * (l + x1[0]) + p * y1[0];
            const double fr = b * (r + x1[1]) + p * y1[1];
            x1 = { l, r };
            y1 = { fl, fr };

            cursor = (cursor + 1) & (CROSSFEED_CAPACITY - 1);
            ring[size_t(cursor) * 2] = fl;
            ring[size_t(cursor) * 2 + 1] = fr;
            if (written < CROSSFEED_CAPACITY) written++;

            into_left = read(delay, 1);
            into_right = read(delay, 0);
            if (delay_left) {
               const double head_blend = 1.0 - double(delay_left) / double(ramp_frames);
               const double old_left = read(old_delay, 1), old_right = read(old_delay, 0);
               into_left = old_left + (into_left - old_left) * head_blend;
               into_right = old_right + (into_right - old_right) * head_blend;
            }

            // The ring is retired once every value that a read head can reach is below the residual.

            if (silent and (std::max(std::abs(fl), std::abs(fr)) <= std::max(reference * CROSSFEED_RESIDUAL, 1e-30))) {
               quiet++;
            }
            else quiet = 0;
         }

         const double a = amount.Value;
         const double scale = trim.Value / (1.0 + a);
         io[0] = float((l + a * into_left) * scale);
         io[1] = float((r + a * into_right) * scale);

         if (ramp_left) {
            --ramp_left;
            amount.advance(not ramp_left); pole.advance(not ramp_left); trim.advance(not ramp_left);
         }

         if (delay_left) --delay_left;

         if (live and (quiet > reach)) retire();
      }
   }
};
