#pragma once

// Mid/side width, balance and trim for the AudioStereo processor.
//
// For stereo input L, R with swap sign v (+1, or -1 when swapped), balance gains bl, br and trim t:
//
//   M = (L + R) / 2,  S = (L - R) / 2
//   S' = v * (a * S + d * HP(S))
//   L' = t * bl * (M + S'),  R' = t * br * (M - S')
//
// Without split, a = w = width / 100 and d = 0.  Exchanging L and R leaves M unchanged and negates S, so the swap is a
// sign on S.  Because every later stage is linear in S, applying the sign after the width stage is identical to
// swapping before it, and leaves the filter history untouched by a swap.
//
// Split.  HP is a 2nd-order Butterworth high-pass at the crossover.  With S_high = HP(S) and S_low = S - S_high, the
// split result b * S_low + w * S_high equals b * S + (w - b) * HP(S), where b = bass_width / 100, so a = b and
// d = w - b.  Equal widths therefore reproduce the unsplit result exactly, and M is never filtered.  Filtering the
// high band rather than the low band places the steep 12 dB/octave slope below the crossover, where it rejects the
// side bass for mono bass; the complementary low band has the shallow slope, above the crossover, where bass_width
// matters least.  The filter runs on every stereo frame whether or not split is enabled, so that enabling it has no
// start-up transient; its output is only heard while d is non-zero.  a + d = w throughout a split toggle, so content
// well above the crossover keeps its width while split ramps in or out.
//
// Balance is linear towards each edge with unity at the centre: a negative balance p scales the right channel by
// 1 + p, a positive balance scales the left by 1 - p.  Mono output is scaled by t only.
//
// Width, split, swap, balance and trim ramp linearly over 10 ms from their current values.  The swap sign therefore
// passes through zero, which is a momentary mono image.  Crossover changes crossfade from the old filter to the new one
// over 10 ms, with the new filter inheriting the old filter's history; changes during a crossfade are coalesced and
// start when it ends.
//
// Tail.  Without split, or with equal widths, the output depends only on the current frame.  Otherwise the filter
// history decays with pole radius r.  The homogeneous response of a 2nd-order section with poles r e^(+-i theta) is
// bounded by about 2 / sin(theta) times its largest history value, and the history is bounded by the L1 norm of the
// high-pass impulse response (below 2.5 for every supported crossover and rate) times the peak side input.
// stereo_tail_bound() doubles both factors for the input history and reports the frames for that bound to fall below
// STEREO_RESIDUAL.  The tests verify both the L1 norm and that measured drains stay within the bound.

#include <algorithm>
#include <cmath>
#include <numbers>

constexpr int STEREO_MIN_RATE = 8000;
constexpr int STEREO_MAX_RATE = 192000;
constexpr double STEREO_RESIDUAL = 1e-9;    // History is discarded below this proportion of the peak side input

struct StereoSettings {
   double Width = 100;     // Percent
   double Balance = 0;     // Percent, signed
   bool Swap = false;
   bool Split = false;
   double Crossover = 150; // Hertz
   double BassWidth = 0;   // Percent
   double Gain = 0;        // Decibels
};

// Butterworth high-pass coefficients, normalised so that a0 is one.

struct StereoFilter {
   double B0 = 0, B1 = 0, B2 = 0, A1 = 0, A2 = 0;
};

// DSP-ready values derived from StereoSettings for one sample rate.  Deriving them performs no allocation.

struct StereoTarget {
   int SampleRate = 0;
   double Side = 1;        // Gain applied to S: the width, or the bass width when split
   double Delta = 0;       // Gain applied to HP(S): width minus bass width when split, else zero
   double Sign = 1;        // -1 when the channels are swapped
   double Left = 1, Right = 1; // Balance gains
   double Trim = 1;        // Linear output gain
   double Crossover = 0;   // Hertz
   StereoFilter Filter;
};

//********************************************************************************************************************
// Bilinear 2nd-order Butterworth high-pass with the -3 dB point prewarped to the crossover.

inline StereoFilter stereo_filter(double Crossover, int Rate)
{
   const double omega = 2.0 * std::numbers::pi * Crossover / double(Rate);
   const double one_plus_cos = 1.0 + std::cos(omega);
   const double alpha = std::sin(omega) / std::numbers::sqrt2; // sin(omega) / (2 Q) with Q = 1 / sqrt(2)
   const double a0 = 1.0 + alpha;
   return StereoFilter {
      .B0 = one_plus_cos * 0.5 / a0,
      .B1 = -one_plus_cos / a0,
      .B2 = one_plus_cos * 0.5 / a0,
      .A1 = -2.0 * std::cos(omega) / a0,
      .A2 = (1.0 - alpha) / a0
   };
}

//********************************************************************************************************************
// Pole radius and angle of the filter.  Butterworth poles are always complex for supported crossovers.

inline void stereo_poles(const StereoFilter &Filter, double &Radius, double &Angle)
{
   Radius = std::sqrt(std::max(Filter.A2, 0.0));
   Angle = (Radius > 0) ? std::acos(std::clamp(-Filter.A1 / (2.0 * Radius), -1.0, 1.0)) : 0;
}

// Conservative frames, after the side input ends, until the filter history is below STEREO_RESIDUAL times the peak side
// input; see the header comment.

inline uint64_t stereo_tail_bound(const StereoFilter &Filter)
{
   double radius, angle;
   stereo_poles(Filter, radius, angle);
   if ((radius <= 0) or (angle <= 0)) return 2;
   const double headroom = 2.0 * 2.5 * 2.0 * 2.0 / std::sin(angle);
   return uint64_t(std::ceil(std::log(headroom / STEREO_RESIDUAL) / -std::log(radius))) + 2;
}

// Typical frames for the split output to decay by 60 dB: the time for the pole envelope to fall by 60 dB.

inline uint64_t stereo_decay_estimate(const StereoFilter &Filter)
{
   double radius, angle;
   stereo_poles(Filter, radius, angle);
   if (radius <= 0) return 2;
   return uint64_t(std::ceil(std::log(0.001) / std::log(radius))) + 2;
}

//********************************************************************************************************************
// Derive the complete processing target.  Returns false for unsupported rates.

inline bool stereo_target(const StereoSettings &Settings, int Rate, StereoTarget &Target)
{
   if ((Rate < STEREO_MIN_RATE) or (Rate > STEREO_MAX_RATE)) return false;
   const double balance = Settings.Balance / 100.0;
   Target.SampleRate = Rate;
   const double width = Settings.Width / 100.0;
   const double bass = Settings.BassWidth / 100.0;
   Target.Side      = Settings.Split ? bass : width;
   Target.Delta     = Settings.Split ? (width - bass) : 0.0;
   Target.Sign      = Settings.Swap ? -1.0 : 1.0;
   Target.Left      = (balance > 0) ? 1.0 - balance : 1.0;
   Target.Right     = (balance < 0) ? 1.0 + balance : 1.0;
   Target.Trim      = std::pow(10.0, Settings.Gain / 20.0);
   Target.Crossover = Settings.Crossover;
   Target.Filter    = stereo_filter(Settings.Crossover, Rate);
   return true;
}

//********************************************************************************************************************

class StereoProcessor final : public AudioEffectProcessor {
public:
   // A value that ramps linearly to End over the remaining frames of the shared ramp.

   struct Ramp {
      double Value = 0, End = 0, Step = 0;

      void snap(double Target) { Value = End = Target; Step = 0; }
      void start(double Target, int Frames) { End = Target; Step = (End - Value) / double(Frames); }
      void advance(bool Last) { Value = Last ? End : Value + Step; }
   };

   // Direct form I history, which carries over unchanged when the coefficients are replaced.

   struct History {
      double X1 = 0, X2 = 0, Y1 = 0, Y2 = 0;

      double run(const StereoFilter &F, double X) {
         const double y = F.B0 * X + F.B1 * X1 + F.B2 * X2 - F.A1 * Y1 - F.A2 * Y2;
         X2 = X1; X1 = X;
         Y2 = Y1; Y1 = y;
         return y;
      }

      double magnitude() const {
         return std::max(std::max(std::abs(X1), std::abs(X2)), std::max(std::abs(Y1), std::abs(Y2)));
      }
   };

   extAudioEffect *Owner;
   StereoSettings Settings;  // Committed parameters, read by reset() under the mixer lock
   StereoTarget Target;      // Derived from Settings for the current rate while active
   int SampleRate = 0;
   int Channels = 1;

private:
   int configured_rate = 0;  // Rate validated by prepare() and published under the mixer lock

   Ramp side, delta, sign, left, right, trim;
   int ramp_left = 0, ramp_frames = 1;

   StereoFilter filter, previous_filter, queued_filter;
   History history, previous_history;
   int fade_left = 0, fade_frames = 1;
   bool queued = false;

   double reference = 0;     // Peak side input magnitude since the history was last cleared
   uint64_t tail_cache = 0, decay_cache = 0;
   bool active = false;
   bool live = false;        // The filter history may hold non-zero values

   void retire() {
      live = false;
      reference = 0;
      history = History();
      previous_history = History();
   }

   // The split output is audible while the bass delta is or will become non-zero.

   bool split_audible() const { return (delta.Value != 0) or (delta.End != 0); }

   // Called under the mixer lock, or from reset().  The bounds cover the slower of the current, outgoing and queued
   // filters, and the remaining fade frames.

   void refresh_bounds() {
      uint64_t tail = stereo_tail_bound(filter), decay = stereo_decay_estimate(filter);
      if (fade_left) {
         tail = std::max(tail, stereo_tail_bound(previous_filter));
         decay = std::max(decay, stereo_decay_estimate(previous_filter));
      }
      if (queued) {
         tail = std::max(tail, stereo_tail_bound(queued_filter));
         decay = std::max(decay, stereo_decay_estimate(queued_filter));
      }
      const auto fades = uint64_t(fade_left) + (queued ? uint64_t(fade_frames) : 0);
      tail_cache = tail + fades + uint64_t(ramp_left);
      decay_cache = decay + fades;
   }

   void start_fade(const StereoFilter &Next) {
      previous_filter = filter;
      previous_history = history;
      filter = Next;
      fade_left = fade_frames;
   }

public:
   class Configuration final : public AudioEffectConfiguration {
   public:
      StereoProcessor *Processor;
      int Rate = 0;

      Configuration(StereoProcessor *Target, int PreparedRate) : Processor(Target), Rate(PreparedRate) { }

      void publish() override {
         auto &p = *Processor;
         p.configured_rate = Rate;
         p.active = false;
         p.retire();
      }

      int64_t latency() const override { return 0; }
   };

   StereoProcessor(extAudioEffect *Effect, const StereoSettings &Initial) : Owner(Effect), Settings(Initial) { }

   // There is no storage to allocate, so preparation only validates the rate.  A zero rate leaves the processor
   // inactive until the output is configured.

   ERR prepare(int PrepareRate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) override {
      if ((PrepareRate > 0) and ((PrepareRate < STEREO_MIN_RATE) or (PrepareRate > STEREO_MAX_RATE))) {
         return ERR::NoSupport;
      }
      Result = std::make_unique<Configuration>(this, PrepareRate);
      return ERR::Okay;
   }

   // Caller holds the mixer lock.  Next is derived on the control thread for the rate that was current at the time,
   // or is null if that rate is unsupported.  Gains ramp over 10 ms from their current values; a crossover change
   // crossfades the filters.  History is retained.

   void update(const StereoSettings &NextSettings, const StereoTarget *Next) {
      Settings = NextSettings;
      if ((not active) or Owner->ResetPending or (not Next) or (Next->SampleRate != SampleRate)) return;

      const bool filter_changed = Next->Crossover != Target.Crossover;
      Target = *Next;
      side.start(Next->Side, ramp_frames);
      delta.start(Next->Delta, ramp_frames);
      sign.start(Next->Sign, ramp_frames);
      left.start(Next->Left, ramp_frames);
      right.start(Next->Right, ramp_frames);
      trim.start(Next->Trim, ramp_frames);
      ramp_left = ramp_frames;

      if (filter_changed) {
         if (fade_left) {
            queued_filter = Next->Filter;
            queued = true;
         }
         else start_fade(Next->Filter);
      }

      refresh_bounds();
   }

   AudioTail tail() const override { return AudioTail::FINITE; }
   uint64_t tail_frames() const override { return (active and (Channels IS 2) and split_audible()) ? tail_cache : 0; }
   uint64_t decay_estimate() const override {
      return (active and (Channels IS 2) and split_audible()) ? decay_cache : 0;
   }
   bool pending() const override { return active and (Channels IS 2) and live and split_audible(); }

   double side_gain() const { return side.Value; }
   double bass_delta() const { return delta.Value; }
   double swap_sign() const { return sign.Value; }
   double left_gain() const { return left.Value; }
   double right_gain() const { return right.Value; }
   double trim_gain() const { return trim.Value; }
   bool fading() const { return fade_left > 0; }
   bool fade_queued() const { return queued; }
   bool history_live() const { return live; }
   const StereoFilter & current_filter() const { return filter; }

   void reset() override {
      SampleRate = Owner->OutputRate;
      Channels = Owner->Stereo ? 2 : 1;
      retire();
      ramp_left = 0;
      fade_left = 0;
      queued = false;

      active = (configured_rate > 0) and (SampleRate IS configured_rate) and (Owner->Layout.size() <= 2) and
         stereo_target(Settings, SampleRate, Target);
      if (not active) return;

      ramp_frames = std::max(1, SampleRate / 100); // 10 ms
      fade_frames = ramp_frames;
      side.snap(Target.Side);
      delta.snap(Target.Delta);
      sign.snap(Target.Sign);
      left.snap(Target.Left);
      right.snap(Target.Right);
      trim.snap(Target.Trim);
      filter = Target.Filter;
      refresh_bounds();
   }

   // Non-finite input is outside the normalised pipeline contract and is treated as silence.  The filter history is
   // discarded once the side input is silent and the history is below the residual, which stops denormal values and
   // ends pending().

   void process(float *Buffer, int Frames) override {
      if (not active) return;

      if (Channels IS 1) {
         for (int frame = 0; frame < Frames; ++frame) {
            const double x = std::isfinite(Buffer[frame]) ? double(Buffer[frame]) : 0.0;
            Buffer[frame] = float(x * trim.Value);
            if (ramp_left) {
               --ramp_left;
               side.advance(not ramp_left); delta.advance(not ramp_left); sign.advance(not ramp_left);
               left.advance(not ramp_left); right.advance(not ramp_left); trim.advance(not ramp_left);
            }
         }
         return;
      }

      for (int frame = 0; frame < Frames; ++frame) {
         float *io = Buffer + size_t(frame) * 2;
         const double l = std::isfinite(io[0]) ? double(io[0]) : 0.0;
         const double r = std::isfinite(io[1]) ? double(io[1]) : 0.0;
         const double m = (l + r) * 0.5;
         const double s = (l - r) * 0.5;

         if (s != 0) {
            live = true;
            reference = std::max(reference, std::abs(s));
         }

         double high = 0;
         if (live) {
            high = history.run(filter, s);
            if (fade_left) {
               const double old = previous_history.run(previous_filter, s);
               high = old + (high - old) * (1.0 - double(fade_left) / double(fade_frames));
            }
         }

         double processed = side.Value * s;
         if (delta.Value != 0) processed += delta.Value * high;
         processed *= sign.Value;

         io[0] = float(trim.Value * left.Value * (m + processed));
         io[1] = float(trim.Value * right.Value * (m - processed));

         if (ramp_left) {
            --ramp_left;
            side.advance(not ramp_left); delta.advance(not ramp_left); sign.advance(not ramp_left);
            left.advance(not ramp_left); right.advance(not ramp_left); trim.advance(not ramp_left);
         }

         if (fade_left) {
            if (not --fade_left) {
               if (queued) {
                  queued = false;
                  start_fade(queued_filter);
               }
            }
         }

         if (live and (s IS 0)) {
            double level = history.magnitude();
            if (fade_left) level = std::max(level, previous_history.magnitude());
            if (level <= std::max(reference * STEREO_RESIDUAL, 1e-30)) retire();
         }
      }
   }
};
