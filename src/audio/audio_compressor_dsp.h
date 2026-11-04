#pragma once

// Feed-forward peak compressor for the AudioCompressor processor.
//
// Signal path, per frame: the detector takes the absolute mono sample, or the greater absolute stereo sample, and
// converts it to dBFS.  A static transfer curve with an optional quadratic soft knee gives the target gain reduction
// in dB, which a one-pole envelope smooths with separate attack and release time constants.  The same gain is applied
// to every channel, followed by makeup gain and a linear blend with the dry input.  There is no lookahead, so the
// gain computed for a frame is applied to that frame.

#include <algorithm>
#include <cmath>
#include <numbers>

constexpr int COMPRESSOR_MIN_RATE = 8000;
constexpr int COMPRESSOR_MAX_RATE = 192000;
constexpr double COMPRESSOR_SNAP = 1e-12;                       // dB; releasing envelopes below this become zero
constexpr double COMPRESSOR_DB_TO_LOG = std::numbers::ln10 / 20.0; // Natural-log gain per dB

struct CompressorSettings {
   double Threshold = -18;  // dBFS
   double Ratio = 4;        // Input/output slope above the threshold
   double Attack = 10;      // Milliseconds, exponential time constant
   double Release = 100;    // Milliseconds, exponential time constant
   double Knee = 6;         // dB; zero is a hard knee
   double Makeup = 0;       // dB, applied to the compressed branch only
   double Mix = 100;        // Percent of the compressed branch
};

// DSP-ready values derived from CompressorSettings for one sample rate.  Every field is continuous and remains valid
// when interpolated between two targets, which allows parameter changes to ramp independently of block size.

struct CompressorTarget {
   int Rate = 0;
   double Threshold = 0;    // dBFS
   double Knee = 0;         // Knee width in dB
   double Slope = 0;        // 1 - 1 / ratio
   double Attack = 0;       // One-pole coefficients
   double Release = 0;
   double Makeup = 1;       // Linear gain
   double Mix = 1;          // Compressed proportion, 0 to 1
};

// Fields that ramp during a parameter change.

static constexpr double CompressorTarget::*glCompressorRamped[] = {
   &CompressorTarget::Threshold, &CompressorTarget::Knee, &CompressorTarget::Slope, &CompressorTarget::Attack,
   &CompressorTarget::Release, &CompressorTarget::Makeup, &CompressorTarget::Mix
};

//********************************************************************************************************************
// Pole of a one-pole smoother whose time constant is Milliseconds: after that time 1 - 1/e of a step is traversed.

inline double compressor_pole(double Milliseconds, int Rate)
{
   return std::exp(-1.0 / (Milliseconds * 0.001 * double(Rate)));
}

//********************************************************************************************************************
// Derive the complete processing target.  Returns false for unsupported rates.

inline bool compressor_target(const CompressorSettings &Settings, int Rate, CompressorTarget &Target)
{
   if ((Rate < COMPRESSOR_MIN_RATE) or (Rate > COMPRESSOR_MAX_RATE)) return false;
   Target.Rate      = Rate;
   Target.Threshold = Settings.Threshold;
   Target.Knee      = Settings.Knee;
   Target.Slope     = 1.0 - 1.0 / Settings.Ratio;
   Target.Attack    = compressor_pole(Settings.Attack, Rate);
   Target.Release   = compressor_pole(Settings.Release, Rate);
   Target.Makeup    = std::pow(10.0, Settings.Makeup / 20.0);
   Target.Mix       = Settings.Mix / 100.0;
   return true;
}

//********************************************************************************************************************
// Static transfer curve: the non-negative gain reduction in dB for a detector Level in dBFS.  The quadratic knee is
// continuous in value and first derivative at both boundaries.  A zero Knee selects the hard knee, so the quadratic
// region, and its division by the knee width, is never reached.

inline double compressor_reduction(double Level, double Threshold, double Knee, double Slope)
{
   if (Knee > 0) {
      const double lower = Threshold - Knee * 0.5;
      if (Level <= lower) return 0;
      if (Level < Threshold + Knee * 0.5) {
         const double over = Level - lower;
         return Slope * over * over / (2.0 * Knee);
      }
   }
   else if (Level <= Threshold) return 0;
   return Slope * (Level - Threshold);
}

//********************************************************************************************************************

class CompressorProcessor final : public AudioEffectProcessor {
public:
   extAudioEffect *Owner;
   CompressorSettings Settings; // Committed parameters, read by reset() under the mixer lock
   CompressorTarget Target;     // Derived from Settings for configured_rate
   int Rate = 0;
   int Channels = 1;

private:
   CompressorTarget current, end, step;
   int configured_rate = 0;     // Rate validated by prepare() and published under the mixer lock
   int ramp_left = 0, ramp_frames = 1;
   double lower_linear = 0;     // Linear magnitude of the current lower knee boundary
   double envelope_db = 0;      // Smoothed gain reduction
   double reduction = 0;        // Maximum envelope during the last process() call
   bool active = false;

   // Magnitudes at or below this cannot reach the knee, so the detector skips the logarithm for them.

   void update_boundary() {
      lower_linear = std::exp((current.Threshold - std::max(current.Knee, 0.0) * 0.5) * COMPRESSOR_DB_TO_LOG);
   }

public:
   class Configuration final : public AudioEffectConfiguration {
   public:
      CompressorProcessor *Processor;
      int Rate = 0;

      Configuration(CompressorProcessor *Target, int PreparedRate) : Processor(Target), Rate(PreparedRate) { }

      // Called under the mixer lock, where the committed settings are stable.  The derivation is constant-time.
      void publish() override {
         auto &p = *Processor;
         p.configured_rate = Rate;
         p.active = false;
         if (Rate > 0) compressor_target(p.Settings, Rate, p.Target);
      }

      int64_t latency() const override { return 0; }
   };

   CompressorProcessor(extAudioEffect *Effect, const CompressorSettings &Initial) : Owner(Effect), Settings(Initial) { }

   // No storage is required.  A zero rate leaves the processor inactive until the output is configured.

   ERR prepare(int PrepareRate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) override {
      if ((PrepareRate > 0) and ((PrepareRate < COMPRESSOR_MIN_RATE) or (PrepareRate > COMPRESSOR_MAX_RATE))) {
         return ERR::NoSupport;
      }
      Result = std::make_unique<Configuration>(this, PrepareRate);
      return ERR::Okay;
   }

   // Caller holds the mixer lock.  Next is derived on the control thread for the rate that was current at the time,
   // or is null if that rate is unsupported.  A live edit ramps every continuous field over 10 ms, starting from the
   // current interpolated values.

   void update(const CompressorSettings &NextSettings, const CompressorTarget *Next) {
      Settings = NextSettings;
      if (configured_rate <= 0) return;
      if (Next and (Next->Rate IS configured_rate)) Target = *Next;
      else compressor_target(Settings, configured_rate, Target);

      if ((not active) or Owner->ResetPending) return;

      end = Target;
      for (auto field : glCompressorRamped) step.*field = (end.*field - current.*field) / double(ramp_frames);
      ramp_left = ramp_frames;
   }

   // The compressor never produces output from silence, so it is never pending and has no tail.

   AudioTail tail() const override { return AudioTail::NONE; }
   double gain_reduction() const override { return reduction; }
   double envelope() const { return envelope_db; }

   void reset() override {
      Rate = Owner->OutputRate;
      Channels = Owner->Stereo ? 2 : 1;
      ramp_left = 0;
      envelope_db = 0;
      reduction = 0;
      active = (configured_rate > 0) and (Rate IS configured_rate) and (Owner->Layout.size() <= 2);
      if (not active) return;

      ramp_frames = std::max(1, Rate / 100); // 10 ms
      current = end = Target;
      update_boundary();
   }

   void process(float *Buffer, int Frames) override {
      reduction = 0;
      if (not active) return;

      for (int frame = 0; frame < Frames; ++frame) {
         float *io = Buffer + size_t(frame) * Channels;
         double magnitude = std::abs(double(io[0]));
         if (Channels IS 2) magnitude = std::max(magnitude, std::abs(double(io[1])));

         double target = 0;
         if ((magnitude > lower_linear) and (current.Slope > 0)) {
            target = compressor_reduction(20.0 * std::log10(magnitude), current.Threshold, current.Knee,
               current.Slope);
         }

         const double pole = (target > envelope_db) ? current.Attack : current.Release;
         envelope_db = target + pole * (envelope_db - target);
         if ((target IS 0) and (envelope_db < COMPRESSOR_SNAP)) envelope_db = 0;
         reduction = std::max(reduction, envelope_db);

         // Zero mix leaves the input untouched while the detector and envelope continue to run.

         if (current.Mix > 0) {
            const double gain = (envelope_db > 0) ? std::exp(-envelope_db * COMPRESSOR_DB_TO_LOG) : 1.0;
            const double wet_gain = gain * current.Makeup;
            for (int c = 0; c < Channels; c++) {
               const double dry = io[c];
               io[c] = float(dry + current.Mix * (dry * wet_gain - dry));
            }
         }

         if (ramp_left) {
            if (--ramp_left) {
               for (auto field : glCompressorRamped) current.*field += step.*field;
            }
            else current = end;
            update_boundary();
         }
      }
   }
};
