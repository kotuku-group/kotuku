#pragma once

// Swept all-pass cascade for the AudioPhaser processor.
//
// Every channel feeds six first-order all-pass stages that share one corner frequency.  A sine LFO sweeps the corner
// geometrically about a centre C by up to O octaves either side.  With phase phi, rate r, signed feedback g and the
// cascade output of the previous frame w:
//
//   corner = clamp(C * 2^(O * sin(phi)), PHASER_MIN_CORNER, PHASER_MAX_CORNER * fs)
//   t      = tan(pi * corner / fs),  k = (1 - t) / (1 + t),  q = 2 sqrt(t) / (1 + t)
//   u      = input + g * w
//   stage  : y = q * s - k * u;  s = k * s + q * u;  u = y        (repeated for the six stages)
//   w      = u
//   out    = (1 - mix) * input + mix * w
//   phi    = wrap(phi + 2 pi r / fs)
//
// Each stage is a normalised lattice section with H(z) = (z^-1 - k) / (1 - k z^-1), the bilinear all-pass whose phase
// is -90 degrees at the corner.  The phase starts at zero on reset, so the sweep starts at the centre and rises first.
// It advances once per frame and both channels share it.  Rate, centre, depth, feedback and mix ramp linearly over
// 10 ms from their current values; the rate ramps as a phase increment and the centre ramps in octaves.
//
// Stability.  k^2 + q^2 = 1 for every corner, so each stage maps (s, u) to (s', y) by an orthogonal matrix, whatever
// the corner was on the previous frame.  The cascade is therefore lossless at every frame: |S'|^2 + y^2 = |S|^2 + u^2.
// Without input the stored energy E = |S|^2 + w^2 becomes |S|^2 + g^2 w^2, so E never increases for |g| <= 1, however
// the coefficients vary.  The wet output never exceeds sqrt(E), which is what pending() measures.
//
// Lossless stages alone do not prove decay, so phaser_contraction() derives an exponential rate from the weighted
// energy V = sum(a_i s_i^2) + w^2 with a_i = 1 + (7 - i) d.  Expanding the stage identities gives
//
//   V' = sum(a_i s_i^2) + a_1 u^2 - d * sum(y_i^2),  i = 1..6
//
// and q_i s_i = y_i + k_i y_(i-1) gives q^2 s_i^2 <= 2 (y_i^2 + y_(i-1)^2) for the smallest q on the trajectory.
// Summing, sum(y_i^2) >= q^2/4 * sum(s_i^2) - u^2/2, so without input (u = g w):
//
//   V' <= (1 - d q^2 / (4 a_1)) * sum(a_i s_i^2) + g^2 (a_1 + d/2) w^2 <= rho * V
//
// The bound holds for any sequence of corners whose q is at least the smallest and any feedback of at most |g|,
// including ramps.  Since E <= V <= a_1 E, every trajectory decays by at least rho per frame up to a factor a_1, and
// the sweep cannot destabilise the loop at maximum feedback.  The same argument bounds the stored state for bounded
// input by sqrt(a_1) / (1 - sqrt(rho)) times the input peak (see phaser_headroom()).  These bounds are conservative:
// at a static 25 Hz corner, 48 kHz and 80% feedback the energy falls 120 dB below the input in 4.2 s, against a
// bound of 11.9 s.
//
// State is kept in double precision, which leaves rounding errors more than ten orders of magnitude below the
// contraction margin.  History is cleared as soon as E falls below the residual bound, so reset() is constant-time.

#include <algorithm>
#include <cmath>
#include <numbers>

constexpr int PHASER_MIN_RATE = 8000;
constexpr int PHASER_MAX_RATE = 192000;
constexpr int PHASER_STAGES = 6;
constexpr double PHASER_MIN_CORNER = 20;      // Hertz; the lowest published sweep reaches 25 Hz
constexpr double PHASER_MAX_CORNER = 0.45;    // Proportion of the sample rate, safely below Nyquist
constexpr double PHASER_MAX_OCTAVES = 2;      // Sweep either side of the centre at 100% depth
constexpr double PHASER_MAX_FEEDBACK = 0.8;   // Matches the published feedback magnitude limit of 80%
constexpr double PHASER_RESIDUAL = 1e-6;      // Residual bound relative to the peak input (-120 dB)
constexpr double PHASER_INPUT_LIMIT = 1e30;   // Input magnitudes are clamped here; bounded state still fits a float

struct PhaserSettings {
   double Rate = 0.4;     // Hertz
   double Centre = 800;   // Hertz
   double Depth = 60;     // Percent of two octaves
   double Feedback = 20;  // Percent, signed
   double Mix = 50;       // Percent
};

// DSP-ready values derived from PhaserSettings for one sample rate.  Deriving them performs no allocation.

struct PhaserTarget {
   int SampleRate = 0;
   double Increment = 0;  // LFO phase advance per frame, in radians
   double Centre = 0;     // Base-2 logarithm of the centre frequency in Hertz
   double Octaves = 0;    // Sweep either side of the centre
   double Feedback = 0;   // Signed linear gain per pass
   double Mix = 0;        // Wet proportion, 0 to 1
};

//********************************************************************************************************************
// The instantaneous corner in Hertz for a base-2 log centre, an octave excursion and a sweep position from -1 to 1.

inline double phaser_corner(double Centre, double Octaves, double Sweep, int Rate)
{
   return std::clamp(std::exp2(Centre + Octaves * Sweep), PHASER_MIN_CORNER, PHASER_MAX_CORNER * double(Rate));
}

//********************************************************************************************************************
// Lattice coefficients for a corner in Hertz.  q is the smallest at the lowest and highest corners, peaking at fs/4.

inline void phaser_coefficients(double Corner, int Rate, double &K, double &Q)
{
   const double t = std::tan(std::numbers::pi * Corner / double(Rate));
   K = (1.0 - t) / (1.0 + t);
   Q = 2.0 * std::sqrt(t) / (1.0 + t);
}

inline double phaser_q(double Corner, int Rate)
{
   double k, q;
   phaser_coefficients(Corner, Rate, k, q);
   return q;
}

//********************************************************************************************************************
// The weighted-energy contraction for a smallest q and a feedback magnitude bound g; see the header comment.  The
// weight step d is chosen so that the feedback term contracts by 0.9 + 0.1 g^2, leaving the state term to set the rate.
// LogRate is -ln(rho) and Weight is a_1, the largest ratio between V and the stored energy.

struct PhaserContraction {
   double LogRate;
   double Weight;
};

inline PhaserContraction phaser_contraction(double Q, double Feedback)
{
   const double g2 = std::min(Feedback * Feedback, PHASER_MAX_FEEDBACK * PHASER_MAX_FEEDBACK);
   const double d = (g2 > 0) ? std::min(10.0, 0.9 * (1.0 - g2) / (6.5 * g2)) : 10.0;
   const double weight = 1.0 + 6.0 * d;
   const double state = -std::log1p(-d * Q * Q / (4.0 * weight));
   const double loop = (g2 > 0) ? -std::log(g2 * (1.0 + 6.5 * d)) : state;
   return PhaserContraction { .LogRate = std::min(state, loop), .Weight = weight };
}

//********************************************************************************************************************
// Upper bound on sqrt(V) relative to the input peak, for any sequence of supported settings at this rate: the worst
// q in the clamped corner range and the maximum feedback.  It is approximately 35000 at 48 kHz and 140000 at 192 kHz.

inline double phaser_headroom(int Rate)
{
   const double q = std::min(phaser_q(PHASER_MIN_CORNER, Rate), phaser_q(PHASER_MAX_CORNER * double(Rate), Rate));
   const auto c = phaser_contraction(q, PHASER_MAX_FEEDBACK);
   return std::sqrt(c.Weight) / -std::expm1(-0.5 * c.LogRate);
}

//********************************************************************************************************************
// Conservative frames, after input ends, until the stored energy is at most (PHASER_RESIDUAL * peak input)^2, for
// corners whose q is at least Q and a feedback magnitude of at most Feedback.  The stored energy at the end of the
// input is bounded by phaser_headroom() under any earlier settings; from there V decays by rho per frame under the
// current settings.  1% is added for rounding.

inline uint64_t phaser_decay_bound(int Rate, double Q, double Feedback)
{
   const auto c = phaser_contraction(Q, Feedback);
   const double headroom = phaser_headroom(Rate);
   const double ratio = c.Weight * headroom * headroom / (PHASER_RESIDUAL * PHASER_RESIDUAL);
   return uint64_t(std::ceil(std::log(ratio) / c.LogRate * 1.01)) + 1;
}

//********************************************************************************************************************
// Typical frames for a 60 dB decay at the lowest corner reached.  Without feedback the cascade rings for about 1.5
// times its DC group delay of 6 / t frames, and each recirculation adds the group delay at the corner,
// 6 (1 + t^2) / (2 t), plus the feedback frame.  Against measured impulse and noise-burst decays at static corners
// from 25 Hz to 1 kHz, this is within 10% below and up to three times above for feedback of up to 80%; the larger
// overestimates occur at low corners with strong feedback, where the sweep rarely dwells.

inline uint64_t phaser_decay_estimate(double Corner, int Rate, double Feedback)
{
   const double t = std::tan(std::numbers::pi * Corner / double(Rate));
   const double g = std::abs(Feedback);
   double repeats = 0;
   if ((g > 0) and (g < 1)) repeats = std::ceil(std::log(0.001) / std::log(g));
   const double stages = double(PHASER_STAGES);
   return uint64_t(std::ceil(1.5 * stages / t + repeats * (stages * (1.0 + t * t) / (2.0 * t) + 1.0)));
}

//********************************************************************************************************************
// Derive the complete processing target.  Returns false for unsupported rates.

inline bool phaser_target(const PhaserSettings &Settings, int Rate, PhaserTarget &Target)
{
   if ((Rate < PHASER_MIN_RATE) or (Rate > PHASER_MAX_RATE)) return false;
   Target.SampleRate = Rate;
   Target.Increment  = 2.0 * std::numbers::pi * Settings.Rate / double(Rate);
   Target.Centre     = std::log2(Settings.Centre);
   Target.Octaves    = PHASER_MAX_OCTAVES * Settings.Depth / 100.0;
   Target.Feedback   = std::clamp(Settings.Feedback / 100.0, -PHASER_MAX_FEEDBACK, PHASER_MAX_FEEDBACK);
   Target.Mix        = Settings.Mix / 100.0;
   return true;
}

//********************************************************************************************************************

class PhaserProcessor final : public AudioEffectProcessor {
public:
   extAudioEffect *Owner;
   PhaserSettings Settings;  // Committed parameters, read by reset() under the mixer lock
   PhaserTarget Target;      // Derived from Settings for the current rate while active
   int SampleRate = 0;
   int Channels = 1;

private:
   int configured_rate = 0;  // Rate validated by prepare() and published under the mixer lock

   double state[2][PHASER_STAGES] = {}; // Lattice states per channel
   double loop[2] = {};      // Cascade output of the previous frame, per channel
   double phase = 0;         // LFO phase in radians, in [0, 2 pi)

   // Smoothed values: phase increment, log2 centre, octave excursion, signed feedback gain and wet mix.
   double increment = 0, centre = 0, octaves = 0, gain = 0, mix = 0;
   double increment_end = 0, centre_end = 0, octaves_end = 0, gain_end = 0, mix_end = 0;
   double increment_step = 0, centre_step = 0, octaves_step = 0, gain_step = 0, mix_step = 0;
   int ramp_left = 0, ramp_frames = 1;

   double reference = 0;     // Peak input magnitude since history was last cleared
   double energy = 0;        // Stored energy after the last frame, summed over channels
   uint64_t tail_cache = 0, decay_cache = 0;
   bool active = false;
   bool live = false;        // History may hold non-zero values

   // Discard history once it is inaudible, so that a later mix edit cannot reveal it.  Constant time.

   void retire() {
      live = false;
      reference = 0;
      energy = 0;
      for (auto &channel : state) std::fill(std::begin(channel), std::end(channel), 0.0);
      loop[0] = loop[1] = 0;
   }

   // Bounds for the slowest decay that the current and ramp-target values can produce.  The lowest and highest
   // corners on a ramp are reached at its ends, because the octave offsets that bound them change linearly.  Called on
   // the control thread under the mixer lock, or from reset().

   void refresh_bounds() {
      const double low = std::min(centre - octaves, centre_end - octaves_end);
      const double high = std::max(centre + octaves, centre_end + octaves_end);
      const double low_corner = phaser_corner(low, 0, 0, SampleRate);
      const double high_corner = phaser_corner(high, 0, 0, SampleRate);
      const double q = std::min(phaser_q(low_corner, SampleRate), phaser_q(high_corner, SampleRate));
      const double feedback = std::max(std::abs(gain), std::abs(gain_end));
      tail_cache = phaser_decay_bound(SampleRate, q, feedback);
      decay_cache = phaser_decay_estimate(low_corner, SampleRate, feedback);
   }

public:
   class Configuration final : public AudioEffectConfiguration {
   public:
      PhaserProcessor *Processor;
      int Rate = 0;

      Configuration(PhaserProcessor *Target, int PreparedRate) : Processor(Target), Rate(PreparedRate) { }

      void publish() override {
         auto &p = *Processor;
         p.configured_rate = Rate;
         p.active = false;
         p.retire();
      }

      int64_t latency() const override { return 0; }
   };

   PhaserProcessor(extAudioEffect *Effect, const PhaserSettings &Initial) : Owner(Effect), Settings(Initial) { }

   // The state is a fixed member array, so preparation only validates the rate.  A zero rate leaves the processor
   // inactive until the output is configured.

   ERR prepare(int PrepareRate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) override {
      if ((PrepareRate > 0) and ((PrepareRate < PHASER_MIN_RATE) or (PrepareRate > PHASER_MAX_RATE))) {
         return ERR::NoSupport;
      }
      Result = std::make_unique<Configuration>(this, PrepareRate);
      return ERR::Okay;
   }

   // Caller holds the mixer lock.  Next is derived on the control thread for the rate that was current at the time,
   // or is null if that rate is unsupported.  Every value ramps over 10 ms from its current value.  History and phase
   // are retained.

   void update(const PhaserSettings &NextSettings, const PhaserTarget *Next) {
      Settings = NextSettings;
      if ((not active) or Owner->ResetPending or (not Next) or (Next->SampleRate != SampleRate)) return;

      Target = *Next;
      increment_end = Next->Increment;
      centre_end = Next->Centre;
      octaves_end = Next->Octaves;
      gain_end = Next->Feedback;
      mix_end = Next->Mix;
      increment_step = (increment_end - increment) / double(ramp_frames);
      centre_step = (centre_end - centre) / double(ramp_frames);
      octaves_step = (octaves_end - octaves) / double(ramp_frames);
      gain_step = (gain_end - gain) / double(ramp_frames);
      mix_step = (mix_end - mix) / double(ramp_frames);
      ramp_left = ramp_frames;
      refresh_bounds();
   }

   AudioTail tail() const override { return AudioTail::FINITE; }
   uint64_t tail_frames() const override { return active ? tail_cache : 0; }
   uint64_t decay_estimate() const override { return active ? decay_cache : 0; }
   bool pending() const override { return active and live; }

   double lfo_phase() const { return phase; }
   double lfo_increment() const { return increment; }
   double centre_log2() const { return centre; }
   double sweep_octaves() const { return octaves; }
   double feedback_gain() const { return gain; }
   double wet_mix() const { return mix; }
   double stored_energy() const { return energy; }
   double corner() const { return phaser_corner(centre, octaves, std::sin(phase), SampleRate); }

   void reset() override {
      SampleRate = Owner->OutputRate;
      Channels = Owner->Stereo ? 2 : 1;
      retire();
      phase = 0;
      ramp_left = 0;

      active = (configured_rate > 0) and (SampleRate IS configured_rate) and (Owner->Layout.size() <= 2) and
         phaser_target(Settings, SampleRate, Target);
      if (not active) return;

      ramp_frames = std::max(1, SampleRate / 100); // 10 ms
      increment = increment_end = Target.Increment;
      centre = centre_end = Target.Centre;
      octaves = octaves_end = Target.Octaves;
      gain = gain_end = Target.Feedback;
      mix = mix_end = Target.Mix;
      increment_step = centre_step = octaves_step = gain_step = mix_step = 0;
      refresh_bounds();
   }

   // Non-finite input is outside the normalised pipeline contract and is treated as silence on entry.  Finite input
   // beyond PHASER_INPUT_LIMIT is clamped so that the bounded state cannot overflow float output.  While no history
   // is live the cascade holds zeros and the input is silent, so the coefficients and the cascade are skipped.

   void process(float *Buffer, int Frames) override {
      if (not active) return;

      for (int frame = 0; frame < Frames; ++frame) {
         float *io = Buffer + size_t(frame) * Channels;

         double dry[2], wet[2] = { 0, 0 };
         bool input_set = false;
         for (int c = 0; c < Channels; c++) {
            double x = std::isfinite(io[c]) ? double(io[c]) : 0.0;
            x = std::clamp(x, -PHASER_INPUT_LIMIT, PHASER_INPUT_LIMIT);
            dry[c] = x;
            input_set |= x != 0;
            reference = std::max(reference, std::abs(x));
         }

         if (input_set) live = true;

         if (live) {
            double k, q;
            phaser_coefficients(phaser_corner(centre, octaves, std::sin(phase), SampleRate), SampleRate, k, q);
            energy = 0;
            for (int c = 0; c < Channels; c++) {
               double u = dry[c] + gain * loop[c];
               for (auto &s : state[c]) {
                  const double y = q * s - k * u;
                  s = k * s + q * u;
                  energy += s * s;
                  u = y;
               }
               loop[c] = u;
               wet[c] = u;
               energy += u * u;
            }
         }

         for (int c = 0; c < Channels; c++) io[c] = float((1.0 - mix) * dry[c] + mix * wet[c]);

         phase += increment;
         if (phase >= 2.0 * std::numbers::pi) phase -= 2.0 * std::numbers::pi;

         if (ramp_left) {
            if (--ramp_left) {
               increment += increment_step;
               centre += centre_step;
               octaves += octaves_step;
               gain += gain_step;
               mix += mix_step;
            }
            else {
               increment = increment_end;
               centre = centre_end;
               octaves = octaves_end;
               gain = gain_end;
               mix = mix_end;
            }
         }

         // Without input E cannot increase, so the wet output stays below the residual once E reaches it.

         if (live) {
            const double residual = std::max(reference * PHASER_RESIDUAL, 1e-30);
            if (energy <= residual * residual) retire();
         }
      }
   }
};
