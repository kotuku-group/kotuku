#pragma once

// Oversampled waveshaper for the AudioSaturator processor.
//
// Every channel is processed independently through the same fixed path:
//
//   input -> 4x interpolation FIR -> drive -> waveshaper -> decimation FIR -> trim -> wet
//   input -> 64-frame delay ----------------------------------------------------------> dry
//   output = (1 - mix) * dry + mix * wet
//
// The waveshapers are soft(z) = tanh(z) and hard(z) = clamp(z, -1, 1).  Both are odd, map zero to zero and have unity
// slope at the origin.  No normalisation or automatic gain compensation is applied.
//
// Both FIR filters are derived from one 257-tap Kaiser-windowed sinc prototype at four times the sample rate, with a
// cutoff of 0.45 fs and beta 10.  The prototype sums to one and each of its four polyphase components is normalised to
// sum to exactly 1/4, so that every interpolation phase passes DC at unity.  Interpolation uses the prototype scaled
// by four, evaluated as four 65-tap polyphase dot products per input frame; decimation uses the prototype at unity
// gain, evaluated once per output frame at phase zero.  The combined linear group delay is (257 + 257 - 2) / 8 = 64
// output frames, and one input frame influences 129 output frames (itself and the 128 that follow).
//
// Drive and the shape blend ramp at the oversampled rate, trim and mix at the base rate, all over 10 ms and ending at
// the same output frame.  Drive and trim ramp in dB; mix and the shape blend ramp linearly.  A shape change crossfades
// the two waveshaper outputs ahead of the shared decimation filter.

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

constexpr int SATURATOR_MIN_RATE = 8000;
constexpr int SATURATOR_MAX_RATE = 192000;
constexpr int SATURATOR_FACTOR = 4;          // Oversampling factor
constexpr int SATURATOR_TAPS = 257;          // Prototype length at the oversampled rate
constexpr int SATURATOR_PHASE_TAPS = 65;     // Taps per interpolation phase, zero-padded where shorter
constexpr int SATURATOR_LATENCY = 64;        // Output frames: (SATURATOR_TAPS * 2 - 2) / (2 * SATURATOR_FACTOR)
constexpr int SATURATOR_SUPPORT = 129;       // Output frames influenced by one input frame, inclusive
constexpr int SATURATOR_HISTORY = 260;       // Oversampled history: 65 frames, covering the decimation window
constexpr double SATURATOR_CUTOFF = 0.45;    // Prototype cutoff as a fraction of the base sample rate
constexpr double SATURATOR_BETA = 10.0;      // Kaiser window parameter
constexpr double SATURATOR_SILENCE = 1e-30;  // Input magnitudes below this are treated as silence
constexpr double SATURATOR_DB_TO_LOG = std::numbers::ln10 / 20.0;

enum { SATURATE_SOFT = 0, SATURATE_HARD = 1 };

struct SaturatorSettings {
   double Drive = 6;          // dB
   int Shape = SATURATE_SOFT;
   double Gain = -6;          // dB
   double Mix = 100;          // Percent
};

// DSP-ready values derived from SaturatorSettings for one sample rate.  Deriving them performs no allocation.

struct SaturatorTarget {
   int SampleRate = 0;
   double Drive = 0;          // dB
   double DriveGain = 1;      // Linear
   double Blend = 0;          // Weight of the hard waveshaper, 0 or 1
   double Gain = 0;           // dB
   double TrimGain = 1;       // Linear
   double Mix = 1;            // Wet proportion, 0 to 1
};

struct SaturatorFilter {
   std::array<double, SATURATOR_TAPS> Prototype; // Decimation coefficients; symmetric with unity sum
   // Interpolation coefficients for each phase, ordered from the oldest input frame to the newest, with gain four.
   std::array<std::array<double, SATURATOR_PHASE_TAPS>, SATURATOR_FACTOR> Phases;
};

//********************************************************************************************************************
// The zeroth-order modified Bessel function of the first kind, by its power series.

inline double saturator_bessel_i0(double X)
{
   double sum = 1, term = 1;
   const double quarter = X * X * 0.25;
   for (int k = 1; k < 200; k++) {
      term *= quarter / (double(k) * double(k));
      sum += term;
      if (term < sum * 1e-17) break;
   }
   return sum;
}

//********************************************************************************************************************
// Design the fixed filters.  The table is computed once, on first use from the control thread, and is immutable
// thereafter.

inline SaturatorFilter saturator_design()
{
   SaturatorFilter filter;
   constexpr int middle = (SATURATOR_TAPS - 1) / 2;
   const double cutoff = SATURATOR_CUTOFF / double(SATURATOR_FACTOR); // Cycles per oversampled frame
   const double window_norm = saturator_bessel_i0(SATURATOR_BETA);

   double sum = 0;
   for (int n = 0; n < SATURATOR_TAPS; n++) {
      const double offset = double(n - middle);
      const double sinc = (n IS middle) ? 1.0 :
         std::sin(2.0 * std::numbers::pi * cutoff * offset) / (2.0 * std::numbers::pi * cutoff * offset);
      const double position = offset / double(middle);
      const double window = saturator_bessel_i0(SATURATOR_BETA * std::sqrt(1.0 - position * position)) / window_norm;
      filter.Prototype[n] = 2.0 * cutoff * sinc * window;
      sum += filter.Prototype[n];
   }

   // Normalise each polyphase component to 1/4.  Phases one and three are mirror images with equal sums, but their
   // sums are accumulated in a different order, so the upper half is restored from the lower half afterwards to keep
   // the prototype exactly symmetric.

   for (int p = 0; p < SATURATOR_FACTOR; p++) {
      double phase_sum = 0;
      for (int n = p; n < SATURATOR_TAPS; n += SATURATOR_FACTOR) phase_sum += filter.Prototype[n];
      const double scale = 1.0 / (double(SATURATOR_FACTOR) * phase_sum);
      for (int n = p; n < SATURATOR_TAPS; n += SATURATOR_FACTOR) filter.Prototype[n] *= scale;
   }
   for (int n = 0; n < middle; n++) filter.Prototype[SATURATOR_TAPS - 1 - n] = filter.Prototype[n];

   // Phase p of the interpolated output for input frame n is sum_j 4 h[4j + p] x[n - j].  With the 65 most recent
   // input frames stored oldest first, the coefficient for window index i is 4 h[4 (64 - i) + p].

   for (int p = 0; p < SATURATOR_FACTOR; p++) {
      for (int i = 0; i < SATURATOR_PHASE_TAPS; i++) {
         const int n = SATURATOR_FACTOR * (SATURATOR_PHASE_TAPS - 1 - i) + p;
         filter.Phases[p][i] = (n < SATURATOR_TAPS) ? double(SATURATOR_FACTOR) * filter.Prototype[n] : 0.0;
      }
   }
   return filter;
}

inline const SaturatorFilter & saturator_filter()
{
   static const SaturatorFilter filter = saturator_design();
   return filter;
}

//********************************************************************************************************************
// Derive the complete processing target.  Returns false for unsupported rates.

inline bool saturator_target(const SaturatorSettings &Settings, int Rate, SaturatorTarget &Target)
{
   if ((Rate < SATURATOR_MIN_RATE) or (Rate > SATURATOR_MAX_RATE)) return false;
   Target.SampleRate = Rate;
   Target.Drive      = Settings.Drive;
   Target.DriveGain  = std::exp(Settings.Drive * SATURATOR_DB_TO_LOG);
   Target.Blend      = (Settings.Shape IS SATURATE_HARD) ? 1.0 : 0.0;
   Target.Gain       = Settings.Gain;
   Target.TrimGain   = std::exp(Settings.Gain * SATURATOR_DB_TO_LOG);
   Target.Mix        = Settings.Mix / 100.0;
   return true;
}

//********************************************************************************************************************
// The waveshaper output for a hard-curve weight of Blend.  At either endpoint only one curve is evaluated.

inline double saturator_shape(double Z, double Blend)
{
   if (Blend <= 0) return std::tanh(Z);
   const double hard = std::clamp(Z, -1.0, 1.0);
   if (Blend >= 1) return hard;
   const double soft = std::tanh(Z);
   return soft + (hard - soft) * Blend;
}

//********************************************************************************************************************
// Four independent partial sums let the compiler vectorise the products without reassociating floating-point
// arithmetic, which is about four times faster than a single accumulator.  The summation order is fixed, so results
// do not depend on block partitioning.

inline double saturator_dot(const double *Coefficients, const double *Values, int Count)
{
   double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
   int i = 0;
   for (; i + 4 <= Count; i += 4) {
      s0 += Coefficients[i] * Values[i];
      s1 += Coefficients[i + 1] * Values[i + 1];
      s2 += Coefficients[i + 2] * Values[i + 2];
      s3 += Coefficients[i + 3] * Values[i + 3];
   }
   for (; i < Count; i++) s0 += Coefficients[i] * Values[i];
   return (s0 + s1) + (s2 + s3);
}

//********************************************************************************************************************

class SaturatorProcessor final : public AudioEffectProcessor {
public:
   extAudioEffect *Owner;
   SaturatorSettings Settings; // Committed parameters, read by reset() under the mixer lock
   SaturatorTarget Target;     // Derived from Settings for the current rate while active
   int SampleRate = 0;
   int Channels = 1;

private:
   const SaturatorFilter *filter = nullptr;

   // Storage prepared for storage_channels, swapped in by Configuration::publish().  Each history is duplicated
   // so that every FIR window is contiguous.
   std::vector<double> input;  // 2 * SATURATOR_PHASE_TAPS per channel
   std::vector<double> shaped; // 2 * SATURATOR_HISTORY per channel, at the oversampled rate
   std::vector<float> dry;     // SATURATOR_LATENCY per channel
   int storage_rate = 0, storage_channels = 0;

   int input_pos = 0;          // Slot of the newest input frame
   int shaped_pos = 0;         // Slot of phase zero of the newest frame, a multiple of four
   int dry_pos = 0;            // Slot of the oldest dry frame, which is replaced by the next input
   int countdown = 0;          // Frames after the current one for which non-zero output may remain

   // Oversampled controls: drive in dB and as a linear gain, and the hard-curve weight.
   double drive_db = 0, drive_gain = 1, blend = 0;
   double drive_end = 0, drive_gain_end = 1, blend_end = 0;
   double drive_step = 0, blend_step = 0;
   int fine_left = 0, fine_frames = 4;

   // Base-rate controls: trim in dB and as a linear gain, and the wet mix.
   double trim_db = 0, trim_gain = 1, mix = 1;
   double trim_end = 0, trim_gain_end = 1, mix_end = 1;
   double trim_step = 0, mix_step = 0;
   int ramp_left = 0, ramp_frames = 1;
   bool active = false;

   void clear() {
      std::fill(input.begin(), input.end(), 0.0);
      std::fill(shaped.begin(), shaped.end(), 0.0);
      std::fill(dry.begin(), dry.end(), 0.0f);
      input_pos = shaped_pos = dry_pos = 0;
      countdown = 0;
   }

   // Advance the oversampled controls by one oversampled frame.

   void step_fine() {
      if (not fine_left) return;
      if (--fine_left) {
         drive_db += drive_step;
         drive_gain = std::exp(drive_db * SATURATOR_DB_TO_LOG);
         blend += blend_step;
      }
      else {
         drive_db = drive_end;
         drive_gain = drive_gain_end;
         blend = blend_end;
      }
   }

   void step_base() {
      if (not ramp_left) return;
      if (--ramp_left) {
         trim_db += trim_step;
         trim_gain = std::exp(trim_db * SATURATOR_DB_TO_LOG);
         mix += mix_step;
      }
      else {
         trim_db = trim_end;
         trim_gain = trim_gain_end;
         mix = mix_end;
      }
   }

   void apply_target() {
      drive_db = drive_end = Target.Drive;
      drive_gain = drive_gain_end = Target.DriveGain;
      blend = blend_end = Target.Blend;
      trim_db = trim_end = Target.Gain;
      trim_gain = trim_gain_end = Target.TrimGain;
      mix = mix_end = Target.Mix;
      drive_step = blend_step = trim_step = mix_step = 0;
      fine_left = ramp_left = 0;
   }

public:
   class Configuration final : public AudioEffectConfiguration {
   public:
      SaturatorProcessor *Processor;
      const SaturatorFilter *Filter = nullptr;
      std::vector<double> Input, Shaped;
      std::vector<float> Dry;
      int Rate = 0, Channels = 0;

      explicit Configuration(SaturatorProcessor *Target) : Processor(Target) { }

      // Swap under the mixer lock.  The processor's previous storage is retired here and freed after unlocking.
      void publish() override {
         auto &p = *Processor;
         p.input.swap(Input);
         p.shaped.swap(Shaped);
         p.dry.swap(Dry);
         if (Filter) p.filter = Filter;
         p.storage_rate = Rate;
         p.storage_channels = Channels;
         p.active = false;
         p.countdown = 0;
         p.input_pos = p.shaped_pos = p.dry_pos = 0;
      }

      int64_t latency() const override { return (Rate > 0) ? SATURATOR_LATENCY : 0; }
   };

   SaturatorProcessor(extAudioEffect *Effect, const SaturatorSettings &Initial) : Owner(Effect), Settings(Initial) { }

   // Storage depends only on the channel count, so parameter and rate changes within the supported range never
   // require a different design.  The filter table is built here on first use, outside the mixer lock.  A zero rate
   // leaves the processor inactive, with no latency, until the output is configured.

   ERR prepare(int PrepareRate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) override {
      auto config = std::make_unique<Configuration>(this);
      if (PrepareRate > 0) {
         if ((PrepareRate < SATURATOR_MIN_RATE) or (PrepareRate > SATURATOR_MAX_RATE)) return ERR::NoSupport;
         config->Rate = PrepareRate;
         config->Channels = Stereo ? 2 : 1;
         config->Filter = &saturator_filter();
         config->Input.assign(size_t(2 * SATURATOR_PHASE_TAPS) * config->Channels, 0.0);
         config->Shaped.assign(size_t(2 * SATURATOR_HISTORY) * config->Channels, 0.0);
         config->Dry.assign(size_t(SATURATOR_LATENCY) * config->Channels, 0.0f);
      }
      Result = std::move(config);
      return ERR::Okay;
   }

   // Caller holds the mixer lock.  Next is derived on the control thread for the rate that was current at the time,
   // or is null if that rate is unsupported.  Every control ramps over 10 ms from its current value, so repeated edits
   // retarget smoothly.  Filter histories and the dry delay are retained.

   void update(const SaturatorSettings &NextSettings, const SaturatorTarget *Next) {
      Settings = NextSettings;
      if ((not active) or Owner->ResetPending or (not Next) or (Next->SampleRate != SampleRate)) return;

      Target = *Next;
      drive_end = Next->Drive;
      drive_gain_end = Next->DriveGain;
      blend_end = Next->Blend;
      drive_step = (drive_end - drive_db) / double(fine_frames);
      blend_step = (blend_end - blend) / double(fine_frames);
      fine_left = fine_frames;

      trim_end = Next->Gain;
      trim_gain_end = Next->TrimGain;
      mix_end = Next->Mix;
      trim_step = (trim_end - trim_db) / double(ramp_frames);
      mix_step = (mix_end - mix) / double(ramp_frames);
      ramp_left = ramp_frames;
   }

   // The tail covers the full support of the filters and dry delay, which includes the latency, and is independent of
   // the settings so that a later mix edit can never reveal output after the processor has stopped reporting it.

   AudioTail tail() const override { return AudioTail::FINITE; }
   uint64_t tail_frames() const override { return active ? uint64_t(SATURATOR_SUPPORT) : 0; }
   uint64_t decay_estimate() const override {
      return active ? uint64_t(SATURATOR_SUPPORT - SATURATOR_LATENCY) : 0;
   }
   int64_t latency() const override { return (storage_rate > 0) ? SATURATOR_LATENCY : 0; }
   bool pending() const override { return active and (countdown > 0); }

   double drive_level() const { return drive_db; }
   double drive_linear() const { return drive_gain; }
   double hard_weight() const { return blend; }
   double trim_level() const { return trim_db; }
   double trim_linear() const { return trim_gain; }
   double wet_mix() const { return mix; }
   bool ramping() const { return (fine_left > 0) or (ramp_left > 0); }
   size_t storage_samples() const { return input.size() + shaped.size() + dry.size(); }
   size_t storage_bytes() const {
      return (input.size() + shaped.size()) * sizeof(double) + dry.size() * sizeof(float);
   }
   const double *shaped_storage() const { return shaped.data(); }

   void reset() override {
      SampleRate = Owner->OutputRate;
      Channels = Owner->Stereo ? 2 : 1;
      clear();

      active = (storage_rate > 0) and filter and (SampleRate IS storage_rate) and (Channels <= storage_channels) and
         (Owner->Layout.size() <= 2) and saturator_target(Settings, SampleRate, Target);
      if (not active) return;

      ramp_frames = std::max(1, SampleRate / 100); // 10 ms
      fine_frames = ramp_frames * SATURATOR_FACTOR;
      apply_target();
   }

   // Non-finite input is outside the normalised pipeline contract and, like magnitudes below SATURATOR_SILENCE, is
   // treated as silence on entry.  While the countdown is zero every history is zero, so a silent frame produces
   // silence without evaluating the filters.  Controls still advance, so output is independent of the partition.

   void process(float *Buffer, int Frames) override {
      if (not active) return;

      const auto &coefficients = *filter;
      const double *prototype = coefficients.Prototype.data();
      constexpr int input_span = SATURATOR_PHASE_TAPS;
      constexpr int shaped_span = SATURATOR_HISTORY;

      for (int frame = 0; frame < Frames; ++frame) {
         float *io = Buffer + size_t(frame) * Channels;

         double x[2];
         bool input_set = false;
         for (int c = 0; c < Channels; c++) {
            x[c] = double(io[c]);
            if ((not std::isfinite(x[c])) or (std::abs(x[c]) < SATURATOR_SILENCE)) x[c] = 0;
            else input_set = true;
         }

         if (input_set) countdown = SATURATOR_SUPPORT - 1;
         else if (countdown > 0) countdown--;
         else {
            // Idle: all histories are zero and remain so, so the slot positions are immaterial.
            for (int c = 0; c < Channels; c++) io[c] = 0.0f;
            for (int p = 0; p < SATURATOR_FACTOR; p++) step_fine();
            step_base();
            continue;
         }

         double drive[SATURATOR_FACTOR], weight[SATURATOR_FACTOR];
         for (int p = 0; p < SATURATOR_FACTOR; p++) {
            drive[p] = drive_gain;
            weight[p] = blend;
            step_fine();
         }

         if (++input_pos IS input_span) input_pos = 0;
         shaped_pos += SATURATOR_FACTOR;
         if (shaped_pos IS shaped_span) shaped_pos = 0;

         for (int c = 0; c < Channels; c++) {
            double *in = input.data() + size_t(c) * 2 * input_span;
            in[input_pos] = in[input_pos + input_span] = x[c];
            const double *window = in + input_pos + 1; // The 65 most recent frames, oldest first

            double *hist = shaped.data() + size_t(c) * 2 * shaped_span;
            for (int p = 0; p < SATURATOR_FACTOR; p++) {
               const double up = saturator_dot(coefficients.Phases[p].data(), window, input_span);
               const double value = saturator_shape(up * drive[p], weight[p]);
               hist[shaped_pos + p] = hist[shaped_pos + p + shaped_span] = value;
            }

            // Output frame n is the decimation filter evaluated at phase zero of input frame n, over the 257
            // oversampled values ending there.  The prototype is symmetric, so the window needs no reversal.

            const double wet = saturator_dot(prototype, hist + shaped_pos + shaped_span - (SATURATOR_TAPS - 1),
               SATURATOR_TAPS) * trim_gain;

            float *line = dry.data() + size_t(c) * SATURATOR_LATENCY;
            const double delayed = line[dry_pos];
            line[dry_pos] = float(x[c]);

            io[c] = float((1.0 - mix) * delayed + mix * wet);
         }

         if (++dry_pos IS SATURATOR_LATENCY) dry_pos = 0;
         step_base();
      }
   }
};
