#pragma once

// Downward expander and noise gate for the AudioGate processor.
//
// Signal path, per frame: the detector takes the absolute mono sample, or the greater absolute stereo sample, and
// holds the greatest value seen during the last 25 ms or more, so that a periodic waveform is measured by its peaks
// rather than its zero crossings.  The gate opens when the detector reaches the threshold and closes once the
// detector has remained below the threshold minus the hysteresis for longer than the hold time.  While open the target
// gain reduction is zero.  While closed it is the range in gate mode, or the expansion curve bounded by the range in
// expander mode.  A one-pole envelope smooths the target in dB with separate attack (opening) and release (closing)
// time constants and the same gain is applied to every channel.  There is no lookahead, so the gain computed for a
// frame is applied to that frame.

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

constexpr int GATE_MIN_RATE = 8000;
constexpr int GATE_MAX_RATE = 192000;
constexpr int GATE_WINDOW_BLOCKS = 8;                     // Completed detector sub-blocks retained
constexpr double GATE_WINDOW = 0.025;                     // Seconds; minimum detector window, 20 Hz half-period
constexpr double GATE_SNAP = 1e-12;                       // dB; opening envelopes below this become zero
constexpr double GATE_DB_TO_LOG = std::numbers::ln10 / 20.0; // Natural-log gain per dB

enum { GATE_EXPANDER = 0, GATE_GATE = 1 };

struct GateSettings {
   int Mode = GATE_EXPANDER;
   double Threshold = -45;  // dBFS; opening threshold
   double Ratio = 2;        // Expansion slope below the threshold; ignored in gate mode
   double Range = 60;       // dB; maximum attenuation
   double Hysteresis = 3;   // dB; closing threshold below the opening threshold
   double Attack = 2;       // Milliseconds, exponential time constant
   double Hold = 50;        // Milliseconds
   double Release = 150;    // Milliseconds, exponential time constant
};

// DSP-ready values derived from GateSettings for one sample rate.  Every ramped field is continuous and remains valid
// when interpolated between two targets, which allows parameter changes to ramp independently of block size.  The
// mode is represented by Gate, the weight of the gate curve against the expansion curve, so that mode changes blend.

struct GateTarget {
   int Rate = 0;
   int Hold = 0;            // Frames; adopted immediately rather than ramped
   double Threshold = 0;    // dBFS
   double Hysteresis = 0;   // dB
   double Range = 0;        // dB
   double Slope = 0;        // dB of reduction per dB below the threshold: ratio - 1
   double Attack = 0;       // One-pole coefficients
   double Release = 0;
   double Gate = 0;         // 0 for the expansion curve, 1 for the gate
};

// Fields that ramp during a parameter change.

static constexpr double GateTarget::*glGateRamped[] = {
   &GateTarget::Threshold, &GateTarget::Hysteresis, &GateTarget::Range, &GateTarget::Slope, &GateTarget::Attack,
   &GateTarget::Release, &GateTarget::Gate
};

//********************************************************************************************************************
// Pole of a one-pole smoother whose time constant is Milliseconds: after that time 1 - 1/e of a step is traversed.

inline double gate_pole(double Milliseconds, int Rate)
{
   return std::exp(-1.0 / (Milliseconds * 0.001 * double(Rate)));
}

// Length of one detector sub-block.  The window spans the current partial sub-block and GATE_WINDOW_BLOCKS completed
// ones, so it always holds at least GATE_WINDOW seconds and at most one sub-block more.

inline int gate_block_frames(int Rate)
{
   return std::max(1, int(std::ceil(GATE_WINDOW * double(Rate) / double(GATE_WINDOW_BLOCKS))));
}

//********************************************************************************************************************
// Derive the complete processing target.  Returns false for unsupported rates.

inline bool gate_target(const GateSettings &Settings, int Rate, GateTarget &Target)
{
   if ((Rate < GATE_MIN_RATE) or (Rate > GATE_MAX_RATE)) return false;
   Target.Rate       = Rate;
   Target.Hold       = int(std::lround(Settings.Hold * 0.001 * double(Rate)));
   Target.Threshold  = Settings.Threshold;
   Target.Hysteresis = Settings.Hysteresis;
   Target.Range      = Settings.Range;
   Target.Slope      = Settings.Ratio - 1.0;
   Target.Attack     = gate_pole(Settings.Attack, Rate);
   Target.Release    = gate_pole(Settings.Release, Rate);
   Target.Gate       = (Settings.Mode IS GATE_GATE) ? 1.0 : 0.0;
   return true;
}

//********************************************************************************************************************
// Static expansion curve: the non-negative gain reduction in dB for a detector Level in dBFS while the gate is closed.

inline double gate_expansion(double Level, double Threshold, double Slope, double Range)
{
   if (Level >= Threshold) return 0;
   return std::min(Range, Slope * (Threshold - Level));
}

//********************************************************************************************************************

class GateProcessor final : public AudioEffectProcessor {
public:
   extAudioEffect *Owner;
   GateSettings Settings;   // Committed parameters, read by reset() under the mixer lock
   GateTarget Target;       // Derived from Settings for configured_rate
   int Rate = 0;
   int Channels = 1;

private:
   GateTarget current, end, step;
   std::array<double, GATE_WINDOW_BLOCKS> window {}; // Maxima of the completed detector sub-blocks
   int configured_rate = 0; // Rate validated by prepare() and published under the mixer lock
   int ramp_left = 0, ramp_frames = 1;
   int block_frames = 1, block_left = 1, window_index = 0;
   int hold_frames = 0, hold_left = 0;
   double window_max = 0;   // Greatest value in window
   double block_max = 0;    // Greatest magnitude in the current sub-block
   double open_linear = 0;  // Linear magnitudes of the opening and closing thresholds
   double close_linear = 0;
   double floor_linear = 0; // At or below this magnitude the expansion curve reaches the range
   double envelope_db = 0;  // Smoothed gain reduction
   double reduction = 0;    // Maximum envelope during the last process() call
   bool open = false;
   bool active = false;

   void update_boundary() {
      open_linear  = std::exp(current.Threshold * GATE_DB_TO_LOG);
      close_linear = std::exp((current.Threshold - current.Hysteresis) * GATE_DB_TO_LOG);
      floor_linear = (current.Slope > 0) ?
         std::exp((current.Threshold - current.Range / current.Slope) * GATE_DB_TO_LOG) : 0.0;
   }

   // Target reduction while closed.  The logarithm is only required between the floor and the threshold.

   double closed_reduction(double Detector) const {
      double expansion = 0;
      if ((current.Gate < 1) and (current.Slope > 0)) {
         if (Detector <= floor_linear) expansion = current.Range;
         else expansion = gate_expansion(20.0 * std::log10(Detector), current.Threshold, current.Slope,
            current.Range);
      }
      return expansion + current.Gate * (current.Range - expansion);
   }

public:
   class Configuration final : public AudioEffectConfiguration {
   public:
      GateProcessor *Processor;
      int Rate = 0;

      Configuration(GateProcessor *Target, int PreparedRate) : Processor(Target), Rate(PreparedRate) { }

      // Called under the mixer lock, where the committed settings are stable.  The derivation is constant-time.
      void publish() override {
         auto &p = *Processor;
         p.configured_rate = Rate;
         p.active = false;
         if (Rate > 0) gate_target(p.Settings, Rate, p.Target);
      }

      int64_t latency() const override { return 0; }
   };

   GateProcessor(extAudioEffect *Effect, const GateSettings &Initial) : Owner(Effect), Settings(Initial) { }

   // No storage is required.  A zero rate leaves the processor inactive until the output is configured.

   ERR prepare(int PrepareRate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) override {
      if ((PrepareRate > 0) and ((PrepareRate < GATE_MIN_RATE) or (PrepareRate > GATE_MAX_RATE))) {
         return ERR::NoSupport;
      }
      Result = std::make_unique<Configuration>(this, PrepareRate);
      return ERR::Okay;
   }

   // Caller holds the mixer lock.  Next is derived on the control thread for the rate that was current at the time,
   // or is null if that rate is unsupported.  A live edit ramps every continuous field over 10 ms, starting from the
   // current interpolated values.  A new hold time applies immediately and shortens a hold in progress.

   void update(const GateSettings &NextSettings, const GateTarget *Next) {
      Settings = NextSettings;
      if (configured_rate <= 0) return;
      if (Next and (Next->Rate IS configured_rate)) Target = *Next;
      else gate_target(Settings, configured_rate, Target);

      if ((not active) or Owner->ResetPending) return;

      end = Target;
      for (auto field : glGateRamped) step.*field = (end.*field - current.*field) / double(ramp_frames);
      ramp_left = ramp_frames;
      hold_frames = Target.Hold;
      hold_left = std::min(hold_left, hold_frames);
   }

   // The gate never produces output from silence, so it is never pending and has no tail.

   AudioTail tail() const override { return AudioTail::NONE; }
   double gain_reduction() const override { return reduction; }
   double envelope() const { return envelope_db; }
   bool is_open() const { return open; }

   // The processor starts in the state that a long silence produces: closed, at the reduction for silence.

   void reset() override {
      Rate = Owner->OutputRate;
      Channels = Owner->Stereo ? 2 : 1;
      ramp_left = 0;
      reduction = 0;
      envelope_db = 0;
      open = false;
      hold_left = 0;
      window.fill(0);
      window_max = 0;
      block_max = 0;
      window_index = 0;
      active = (configured_rate > 0) and (Rate IS configured_rate) and (Owner->Layout.size() <= 2);
      if (not active) return;

      ramp_frames = std::max(1, Rate / 100); // 10 ms
      block_frames = block_left = gate_block_frames(Rate);
      hold_frames = Target.Hold;
      current = end = Target;
      update_boundary();
      envelope_db = closed_reduction(0);
   }

   void process(float *Buffer, int Frames) override {
      reduction = 0;
      if (not active) return;

      for (int frame = 0; frame < Frames; ++frame) {
         float *io = Buffer + size_t(frame) * Channels;

         // NaN magnitudes are ignored by the detector because std::max() returns its first argument for them.

         block_max = std::max(block_max, std::abs(double(io[0])));
         if (Channels IS 2) block_max = std::max(block_max, std::abs(double(io[1])));
         const double detector = std::max(window_max, block_max);

         if (detector >= open_linear) {
            open = true;
            hold_left = hold_frames;
         }
         else if (open) {
            if (detector >= close_linear) hold_left = hold_frames;
            else if (hold_left > 0) hold_left--;
            else open = false;
         }

         const double target = open ? 0.0 : closed_reduction(detector);
         const double pole = (target > envelope_db) ? current.Release : current.Attack;
         envelope_db = target + pole * (envelope_db - target);
         if ((target IS 0) and (envelope_db < GATE_SNAP)) envelope_db = 0;
         reduction = std::max(reduction, envelope_db);

         if (envelope_db > 0) {
            const double gain = std::exp(-envelope_db * GATE_DB_TO_LOG);
            for (int c = 0; c < Channels; c++) io[c] = float(double(io[c]) * gain);
         }

         if (not --block_left) {
            window[window_index] = block_max;
            window_index = (window_index + 1) % GATE_WINDOW_BLOCKS;
            window_max = *std::max_element(window.begin(), window.end());
            block_max = 0;
            block_left = block_frames;
         }

         if (ramp_left) {
            if (--ramp_left) {
               for (auto field : glGateRamped) current.*field += step.*field;
            }
            else current = end;
            update_boundary();
         }
      }
   }
};
