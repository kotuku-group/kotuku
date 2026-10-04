#pragma once

// Adaptive loudness levelling for the AudioLoudness processor.
//
// Signal path, per frame: every channel is K-weighted as ITU-R BS.1770 defines it, by a high-frequency pre-filter
// shelf followed by the RLB high-pass, and the square of the result is accumulated into the current 100 ms sub-block.
// When a sub-block completes, momentary (400 ms) and short-term (3 s) loudness are derived from the retained sub-block
// mean squares and the gain targets are recomputed.  The measurement is feed-forward: the input is measured, never
// the output, so the gain cannot influence its own measurement.
//
// The target gain is the difference between the target loudness and the short-term loudness, limited by the maximum
// boost and cut.  While the measured loudness is below the gate, the target is the current gain, which holds the gain.
// The applied gain follows its target in the dB domain through a one-pole smoother whose time constant is the
// response time, updated every frame.  Linked operation measures the programme and applies one gain to every channel.
// Unlinked operation measures every channel as a programme of its own, scaled by the channel count so that balanced
// material receives the same gain in both modes.

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

constexpr int LOUDNESS_MIN_RATE = 8000;
constexpr int LOUDNESS_MAX_RATE = 192000;
constexpr int LOUDNESS_CHANNELS = 2;          // Maximum channels in the processing layout
constexpr int LOUDNESS_MOMENTARY_BLOCKS = 4;  // 400 ms
constexpr int LOUDNESS_SHORT_BLOCKS = 30;     // 3 s
constexpr double LOUDNESS_BLOCK = 0.1;        // Seconds per measurement sub-block
constexpr double LOUDNESS_FLOOR = -120;       // LUFS
constexpr double LOUDNESS_OFFSET = -0.691;    // BS.1770 calibration offset
constexpr double LOUDNESS_LIMIT = 1e6;        // Measured sample magnitudes are clamped to this
constexpr double LOUDNESS_SNAP = 1e-6;        // dB; a gain this close to its target adopts it
constexpr double LOUDNESS_DENORMAL = 1e-30;   // Filter state below this is flushed to zero
constexpr double LOUDNESS_DB_TO_LOG = std::numbers::ln10 / 20.0; // Natural-log gain per dB

// Indexes of the processor-defined meter values.

enum { LOUDNESS_MOMENTARY = 0, LOUDNESS_SHORT_TERM, LOUDNESS_APPLIED_GAIN };

struct LoudnessSettings {
   double Target = -16;    // LUFS
   double MaxBoost = 12;   // dB
   double MaxCut = 12;     // dB
   double Response = 3000; // Milliseconds, exponential time constant
   double Gate = -70;      // LUFS
   bool Link = true;
};

struct LoudnessBiquad {
   double B0 = 1, B1 = 0, B2 = 0, A1 = 0, A2 = 0;
};

// DSP-ready values derived from LoudnessSettings for one sample rate.

struct LoudnessTarget {
   int Rate = 0;
   int BlockFrames = 1;     // Frames per measurement sub-block
   LoudnessBiquad Shelf, HighPass;
   double Pole = 0;         // Gain smoother
   double Target = 0;       // LUFS
   double MaxBoost = 0, MaxCut = 0;
   double Gate = 0;         // LUFS
   bool Link = true;
};

//********************************************************************************************************************
// K-weighting filters designed for any rate from the analogue prototypes of the BS.1770 48 kHz coefficients.  At
// 48 kHz they reproduce the published coefficients.

inline LoudnessBiquad loudness_shelf(int Rate)
{
   constexpr double frequency = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
   const double k = std::tan(std::numbers::pi * frequency / double(Rate));
   const double vh = std::pow(10.0, gain / 20.0);
   const double vb = std::pow(vh, 0.4996667741545416);
   const double a0 = 1.0 + k / q + k * k;
   return LoudnessBiquad {
      .B0 = (vh + vb * k / q + k * k) / a0,
      .B1 = 2.0 * (k * k - vh) / a0,
      .B2 = (vh - vb * k / q + k * k) / a0,
      .A1 = 2.0 * (k * k - 1.0) / a0,
      .A2 = (1.0 - k / q + k * k) / a0
   };
}

inline LoudnessBiquad loudness_highpass(int Rate)
{
   constexpr double frequency = 38.13547087602444, q = 0.5003270373238773;
   const double k = std::tan(std::numbers::pi * frequency / double(Rate));
   const double a0 = 1.0 + k / q + k * k;
   return LoudnessBiquad {
      .B0 = 1.0, .B1 = -2.0, .B2 = 1.0,
      .A1 = 2.0 * (k * k - 1.0) / a0,
      .A2 = (1.0 - k / q + k * k) / a0
   };
}

//********************************************************************************************************************
// BS.1770 loudness of a K-weighted mean square summed over channels, floored at LOUDNESS_FLOOR.

inline double loudness_lufs(double MeanSquare)
{
   if (not (MeanSquare > 0)) return LOUDNESS_FLOOR;
   return std::max(LOUDNESS_FLOOR, LOUDNESS_OFFSET + 10.0 * std::log10(MeanSquare));
}

//********************************************************************************************************************
// Derive the complete processing target.  Returns false for unsupported rates.

inline bool loudness_target(const LoudnessSettings &Settings, int Rate, LoudnessTarget &Target)
{
   if ((Rate < LOUDNESS_MIN_RATE) or (Rate > LOUDNESS_MAX_RATE)) return false;
   Target.Rate        = Rate;
   Target.BlockFrames = std::max(1, int(std::lround(LOUDNESS_BLOCK * double(Rate))));
   Target.Shelf       = loudness_shelf(Rate);
   Target.HighPass    = loudness_highpass(Rate);
   Target.Pole        = std::exp(-1.0 / (Settings.Response * 0.001 * double(Rate)));
   Target.Target      = Settings.Target;
   Target.MaxBoost    = Settings.MaxBoost;
   Target.MaxCut      = Settings.MaxCut;
   Target.Gate        = Settings.Gate;
   Target.Link        = Settings.Link;
   return true;
}

//********************************************************************************************************************

class LoudnessProcessor final : public AudioEffectProcessor {
public:
   extAudioEffect *Owner;
   LoudnessSettings Settings; // Committed parameters, read by reset() under the mixer lock
   LoudnessTarget Target;     // Derived from Settings for configured_rate
   int Rate = 0;
   int Channels = 1;

private:
   struct ChannelState {
      double S1 = 0, S2 = 0;  // Transposed direct form II state of the shelf
      double H1 = 0, H2 = 0;  // and of the high-pass
      double Sum = 0;         // Sum of K-weighted squares in the current sub-block
      std::array<double, LOUDNESS_SHORT_BLOCKS> Blocks {}; // Mean squares of the completed sub-blocks
      double Momentary = LOUDNESS_FLOOR; // Channel loudness scaled to the programme, LUFS
      double ShortTerm = LOUDNESS_FLOOR;
      double Gain = 0;        // Applied gain, dB
      double Goal = 0;        // Gain target, dB
      double Linear = 1;      // Applied gain as a factor
   };

   std::array<ChannelState, LOUDNESS_CHANNELS> state;
   int configured_rate = 0;   // Rate validated by prepare() and published under the mixer lock
   int block_left = 1;
   int block_index = 0;       // Next sub-block slot
   int blocks_filled = 0;     // Completed sub-blocks since reset, up to LOUDNESS_SHORT_BLOCKS
   double momentary = LOUDNESS_FLOOR; // Programme loudness, LUFS
   double short_term = LOUDNESS_FLOOR;
   bool active = false;

   // Derive momentary and short-term loudness from the completed sub-blocks.  Until the windows have filled after a
   // reset, they average the sub-blocks received so far.

   void measure() {
      const int short_count = blocks_filled;
      const int momentary_count = std::min(blocks_filled, LOUDNESS_MOMENTARY_BLOCKS);
      double momentary_total = 0, short_total = 0;
      for (int c = 0; c < Channels; c++) {
         auto &ch = state[c];
         double m = 0, s = 0;
         for (int i = 0; i < short_count; i++) {
            const double value = ch.Blocks[(block_index + LOUDNESS_SHORT_BLOCKS - 1 - i) % LOUDNESS_SHORT_BLOCKS];
            s += value;
            if (i < momentary_count) m += value;
         }
         m /= double(momentary_count);
         s /= double(short_count);
         ch.Momentary = loudness_lufs(m * double(Channels));
         ch.ShortTerm = loudness_lufs(s * double(Channels));
         momentary_total += m;
         short_total += s;
      }
      momentary = loudness_lufs(momentary_total);
      short_term = loudness_lufs(short_total);
   }

   // Recompute the gain targets from the latest measurements.  Below the gate the current gain is held, limited to
   // the current maximum boost and cut.

   void retarget() {
      if (Target.Link) {
         // Hold the mean gain in dB below the gate, so previously unlinked channels can converge without a step.

         double held = 0;
         for (int c = 0; c < Channels; c++) held += state[c].Gain;
         held /= double(Channels);
         const double goal = ((momentary < Target.Gate) or (short_term < Target.Gate)) ?
            held : Target.Target - short_term;
         const double limited = std::clamp(goal, -Target.MaxCut, Target.MaxBoost);
         for (int c = 0; c < Channels; c++) state[c].Goal = limited;
         return;
      }

      for (int c = 0; c < Channels; c++) {
         auto &ch = state[c];
         const double goal = ((ch.Momentary < Target.Gate) or (ch.ShortTerm < Target.Gate)) ?
            ch.Gain : Target.Target - ch.ShortTerm;
         ch.Goal = std::clamp(goal, -Target.MaxCut, Target.MaxBoost);
      }
   }

   void complete_block() {
      for (int c = 0; c < Channels; c++) {
         auto &ch = state[c];
         ch.Blocks[block_index] = ch.Sum / double(Target.BlockFrames);
         ch.Sum = 0;
         for (auto value : { &ch.S1, &ch.S2, &ch.H1, &ch.H2 }) {
            if (std::abs(*value) < LOUDNESS_DENORMAL) *value = 0;
         }
      }
      block_index = (block_index + 1) % LOUDNESS_SHORT_BLOCKS;
      blocks_filled = std::min(blocks_filled + 1, LOUDNESS_SHORT_BLOCKS);
      measure();
      retarget();
   }

public:
   class Configuration final : public AudioEffectConfiguration {
   public:
      LoudnessProcessor *Processor;
      int Rate = 0;

      Configuration(LoudnessProcessor *Target, int PreparedRate) : Processor(Target), Rate(PreparedRate) { }

      // Called under the mixer lock, where the committed settings are stable.  The derivation is constant-time.
      void publish() override {
         auto &p = *Processor;
         p.configured_rate = Rate;
         p.active = false;
         if (Rate > 0) loudness_target(p.Settings, Rate, p.Target);
      }

      int64_t latency() const override { return 0; }
   };

   LoudnessProcessor(extAudioEffect *Effect, const LoudnessSettings &Initial) : Owner(Effect), Settings(Initial) { }

   // No storage is required.  A zero rate leaves the processor inactive until the output is configured.

   ERR prepare(int PrepareRate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) override {
      if ((PrepareRate > 0) and ((PrepareRate < LOUDNESS_MIN_RATE) or (PrepareRate > LOUDNESS_MAX_RATE))) {
         return ERR::NoSupport;
      }
      Result = std::make_unique<Configuration>(this, PrepareRate);
      return ERR::Okay;
   }

   // Caller holds the mixer lock.  Next is derived on the control thread for the rate that was current at the time,
   // or is null if that rate is unsupported.  New targets take effect immediately and the applied gain moves to them
   // at the response rate, so no parameter ramp is needed.  Measurement history is preserved.

   void update(const LoudnessSettings &NextSettings, const LoudnessTarget *Next) {
      Settings = NextSettings;
      if (configured_rate <= 0) return;
      if (Next and (Next->Rate IS configured_rate)) Target = *Next;
      else loudness_target(Settings, configured_rate, Target);

      if ((not active) or Owner->ResetPending) return;
      retarget();
   }

   // Gain and measurement state alone never produce output, so the processor has no tail and is never pending.

   AudioTail tail() const override { return AudioTail::NONE; }

   double meter_value(int Value, int Channel, bool &Floor) const override {
      switch (Value) {
         case LOUDNESS_MOMENTARY:
            Floor = momentary <= LOUDNESS_FLOOR;
            return momentary;
         case LOUDNESS_SHORT_TERM:
            Floor = short_term <= LOUDNESS_FLOOR;
            return short_term;
         case LOUDNESS_APPLIED_GAIN:
            return state[((Channel >= 0) and (Channel < Channels)) ? Channel : 0].Gain;
         default:
            return 0;
      }
   }

   double momentary_loudness() const { return momentary; }
   double short_term_loudness() const { return short_term; }
   double channel_short_term(int Channel) const { return state[Channel].ShortTerm; }
   double gain(int Channel) const { return state[Channel].Gain; }
   double goal(int Channel) const { return state[Channel].Goal; }
   int filled() const { return blocks_filled; }

   // Reset discards the measurement history and returns to unity gain.

   void reset() override {
      Rate = Owner->OutputRate;
      Channels = Owner->Stereo ? 2 : 1;
      state.fill(ChannelState {});
      block_index = 0;
      blocks_filled = 0;
      momentary = LOUDNESS_FLOOR;
      short_term = LOUDNESS_FLOOR;
      active = (configured_rate > 0) and (Rate IS configured_rate) and (Owner->Layout.size() <= 2);
      block_left = active ? Target.BlockFrames : 1;
   }

   void process(float *Buffer, int Frames) override {
      if (not active) return;

      const auto &shelf = Target.Shelf;
      const auto &hp = Target.HighPass;
      const double pole = Target.Pole;

      for (int frame = 0; frame < Frames; ++frame) {
         float *io = Buffer + size_t(frame) * Channels;
         for (int c = 0; c < Channels; c++) {
            auto &ch = state[c];
            const double input = double(io[c]);

            // Non-finite and extreme samples are excluded from measurement so that they cannot poison the filters.

            const double x = std::isfinite(input) ? std::clamp(input, -LOUDNESS_LIMIT, LOUDNESS_LIMIT) : 0.0;
            const double y = shelf.B0 * x + ch.S1;
            ch.S1 = shelf.B1 * x - shelf.A1 * y + ch.S2;
            ch.S2 = shelf.B2 * x - shelf.A2 * y;
            const double z = hp.B0 * y + ch.H1;
            ch.H1 = hp.B1 * y - hp.A1 * z + ch.H2;
            ch.H2 = hp.B2 * y - hp.A2 * z;
            ch.Sum += z * z;

            if (ch.Gain != ch.Goal) {
               ch.Gain = ch.Goal + pole * (ch.Gain - ch.Goal);
               if (std::abs(ch.Gain - ch.Goal) < LOUDNESS_SNAP) ch.Gain = ch.Goal;
               ch.Linear = std::exp(ch.Gain * LOUDNESS_DB_TO_LOG);
            }
            io[c] = float(input * ch.Linear);
         }

         if (not --block_left) {
            complete_block();
            block_left = Target.BlockFrames;
         }
      }
   }
};
