#pragma once

// Feedback-delay network for the AudioReverb processor.
//
// Signal path, per frame: each input channel is written to a pre-delay line, read through one or two read heads,
// then passed through four series Schroeder all-pass diffusers.  The diffused channels are injected into an
// eight-line feedback-delay network whose lines are mixed by a normalised Hadamard matrix.  Every line applies a
// one-pole absorption filter whose DC gain sets the decay time and whose response at REVERB_DAMPING_FREQUENCY sets
// the damping.  Wet output is tapped from the line outputs with orthogonal sign patterns, one per output channel.
//
// The Hadamard matrix is orthogonal and every absorption filter has a magnitude no greater than its DC gain, which is
// below one.  The loop is therefore strictly contractive for all supported parameters, including while coefficients
// are ramping between two such states.

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

constexpr int REVERB_LINES = 8;
constexpr int REVERB_DIFFUSERS = 4;
constexpr int REVERB_MIN_RATE = 8000;
constexpr int REVERB_MAX_RATE = 192000;
constexpr double REVERB_MAX_PREDELAY = 250;         // Milliseconds; matches the published pre_delay maximum
constexpr double REVERB_DAMPING_FREQUENCY = 4000;   // Hertz; reference for the high-frequency decay ratio
constexpr double REVERB_MAX_DIFFUSION = 0.75;       // All-pass coefficient at 100% diffusion
constexpr double REVERB_RESIDUAL = 1e-6;            // Residual bound relative to the peak input (-120 dB)

// Line lengths at 100% size, in milliseconds.  Size scales them by 0.2 + 0.8 * size / 100 before rounding each
// length up to a distinct prime number of frames.

static const double glReverbLineMs[REVERB_LINES] = { 43.1, 47.9, 53.3, 59.7, 66.1, 73.7, 81.1, 89.9 };

// Diffuser lengths in milliseconds, one row per channel so that stereo channels decorrelate.

static const double glReverbDiffuserMs[2][REVERB_DIFFUSERS] = {
   { 1.7, 2.9, 4.3, 6.1 }, { 1.9, 3.1, 4.7, 6.7 }
};

// Output sign patterns are rows of the 8x8 Sylvester Hadamard matrix, so the stereo outputs are mutually orthogonal.

static const double glReverbMonoTap[REVERB_LINES]  = { 1, 1, -1, -1, -1, -1, 1, 1 };  // Row 6
static const double glReverbLeftTap[REVERB_LINES]  = { 1, -1, -1, 1, 1, -1, -1, 1 };  // Row 3
static const double glReverbRightTap[REVERB_LINES] = { 1, -1, 1, -1, -1, 1, -1, 1 };  // Row 5

struct ReverbSettings {
   double Decay = 1500;     // Milliseconds for a 60 dB decay at DC
   double Size = 50;        // Percent
   double PreDelay = 20;    // Milliseconds
   double Damping = 50;     // Percent
   double Diffusion = 70;   // Percent
   double Mix = 20;         // Percent
};

// Coefficients derived from ReverbSettings for one sample rate.  Deriving them performs no allocation.

struct ReverbTarget {
   int Rate = 0;
   std::array<int, REVERB_LINES> Lengths {};
   std::array<double, REVERB_LINES> Gain {}, Pole {};
   double Norm = 0;         // Wet energy normalisation
   double Diffusion = 0;    // All-pass coefficient
   double Mix = 0;          // Wet proportion, 0 to 1
   double DecayFrames = 0;  // Frames for a 60 dB decay at DC
   int PreDelay = 0;        // Frames
};

//********************************************************************************************************************
// The smallest prime at or above Value.  Bounded: prime gaps below 2^31 are far smaller than any line length.

inline int reverb_prime(int Value)
{
   if (Value <= 2) return 2;
   if (not (Value & 1)) Value++;
   for (;; Value += 2) {
      bool prime = true;
      for (int d = 3; prime and (d * d <= Value); d += 2) {
         if (Value % d IS 0) prime = false;
      }
      if (prime) return Value;
   }
}

//********************************************************************************************************************
// Line lengths are monotonic in Size, so the lengths at 100% are the storage capacity of each line.

inline void reverb_lengths(double Size, int Rate, std::array<int, REVERB_LINES> &Lengths)
{
   const double scale = (0.2 + 0.8 * Size / 100.0) * double(Rate) / 1000.0;
   int previous = 0;
   for (int i = 0; i < REVERB_LINES; i++) {
      Lengths[i] = reverb_prime(std::max(previous + 1, int(std::lround(glReverbLineMs[i] * scale))));
      previous = Lengths[i];
   }
}

inline int reverb_diffuser_length(int Channel, int Stage, int Rate)
{
   return reverb_prime(std::max(1, int(std::lround(glReverbDiffuserMs[Channel][Stage] * double(Rate) / 1000.0))));
}

inline int reverb_predelay_capacity(int Rate)
{
   return int(std::lround(REVERB_MAX_PREDELAY * double(Rate) / 1000.0)) + 1;
}

//********************************************************************************************************************
// Pole of a unity-DC one-pole low-pass H(z) = (1 - a) / (1 - a z^-1) whose magnitude at Omega equals Ratio.

inline double reverb_pole(double Ratio, double Omega)
{
   if (Ratio >= 1.0 - 1e-12) return 0;
   const double r2 = Ratio * Ratio;
   const double b = 1.0 - r2 * std::cos(Omega);
   const double c = 1.0 - r2;
   return (b - std::sqrt(std::max(0.0, b * b - c * c))) / c;
}

//********************************************************************************************************************
// Derive the complete processing target.  Returns false for unsupported rates.
//
// Decay: each line's DC gain is 10^(-3 L / (T fs)) for a line of L frames, so every recirculation path decays by
// 60 dB in T seconds regardless of the route taken through the network.
//
// Damping: the high-frequency decay time is T * (1 - 0.9 * damping / 100), evaluated at REVERB_DAMPING_FREQUENCY or
// 40% of the sample rate if that is lower.  Zero damping decays all frequencies equally; 100% damping decays the
// reference frequency ten times faster than DC.
//
// Diffusion: the input all-pass coefficient is 0.75 * diffusion / 100.
//
// Normalisation: the wet output is scaled by sqrt(1 - g^2), where g is the DC gain of a line of mean length.  This
// keeps the wet energy of an undamped impulse response close to that of the input, so decay and size change the
// character of the reverberation rather than its loudness.

inline bool reverb_target(const ReverbSettings &Settings, int Rate, ReverbTarget &Target)
{
   if ((Rate < REVERB_MIN_RATE) or (Rate > REVERB_MAX_RATE)) return false;

   Target.Rate = Rate;
   reverb_lengths(Settings.Size, Rate, Target.Lengths);

   const double decay_frames = Settings.Decay * double(Rate) / 1000.0;
   const double ratio = 1.0 - 0.9 * Settings.Damping / 100.0;
   const double omega = 2.0 * std::numbers::pi * std::min(REVERB_DAMPING_FREQUENCY, double(Rate) * 0.4) /
      double(Rate);

   double mean = 0;
   for (int i = 0; i < REVERB_LINES; i++) {
      const double length = double(Target.Lengths[i]);
      mean += length;
      Target.Gain[i] = std::pow(10.0, -3.0 * length / decay_frames);
      Target.Pole[i] = reverb_pole(std::pow(10.0, -3.0 * length / decay_frames * (1.0 / ratio - 1.0)), omega);
   }
   mean /= double(REVERB_LINES);

   const double mean_gain = std::pow(10.0, -3.0 * mean / decay_frames);
   Target.Norm        = std::sqrt(1.0 - mean_gain * mean_gain);
   Target.Diffusion   = REVERB_MAX_DIFFUSION * Settings.Diffusion / 100.0;
   Target.Mix         = Settings.Mix / 100.0;
   Target.DecayFrames = decay_frames;
   Target.PreDelay    = std::min(int(std::lround(Settings.PreDelay * double(Rate) / 1000.0)),
      reverb_predelay_capacity(Rate) - 1);
   return true;
}

//********************************************************************************************************************
// In-place normalised fast Walsh-Hadamard transform of eight values.

inline void reverb_hadamard(double *Values)
{
   for (int h = 1; h < REVERB_LINES; h <<= 1) {
      for (int i = 0; i < REVERB_LINES; i += h << 1) {
         for (int j = i; j < i + h; j++) {
            const double a = Values[j], b = Values[j + h];
            Values[j] = a + b;
            Values[j + h] = a - b;
         }
      }
   }
   constexpr double scale = 0.35355339059327373; // 1 / sqrt(8)
   for (int i = 0; i < REVERB_LINES; i++) Values[i] *= scale;
}

//********************************************************************************************************************
// One feedback-delay network.  Storage is owned by the processor; every line occupies a fixed region sized for the
// largest supported room, of which the first Length frames are in use.

struct ReverbNetwork {
   std::array<int, REVERB_LINES> Offset {}, Length {}, Cursor {};
   std::array<double, REVERB_LINES> Gain {}, Pole {}, State {};
   std::array<double, REVERB_LINES> GainEnd {}, PoleEnd {}, GainStep {}, PoleStep {};
   double Norm = 0, NormEnd = 0, NormStep = 0;
   int Ramp = 0;

   // Adopt Target immediately.  Clear zeroes the lines in use; the caller may skip it if storage is already clear.

   void configure(float *Storage, const std::array<int, REVERB_LINES> &Offsets, const ReverbTarget &Target,
      bool Clear) {
      Offset = Offsets;
      Length = Target.Lengths;
      Cursor = {};
      State = {};
      Gain = GainEnd = Target.Gain;
      Pole = PoleEnd = Target.Pole;
      Norm = NormEnd = Target.Norm;
      Ramp = 0;
      if (Clear) {
         for (int i = 0; i < REVERB_LINES; i++) std::fill_n(Storage + Offset[i], Length[i], 0.0f);
      }
   }

   // Ramp coefficients linearly towards a target with identical line lengths.  A new target restarts the ramp from
   // the current values.

   void retarget(const ReverbTarget &Target, int Frames) {
      GainEnd = Target.Gain;
      PoleEnd = Target.Pole;
      NormEnd = Target.Norm;
      for (int i = 0; i < REVERB_LINES; i++) {
         GainStep[i] = (GainEnd[i] - Gain[i]) / double(Frames);
         PoleStep[i] = (PoleEnd[i] - Pole[i]) / double(Frames);
      }
      NormStep = (NormEnd - Norm) / double(Frames);
      Ramp = Frames;
   }

   // Process one frame.  Outputs receives the line outputs; Peak is raised to the largest magnitude written.

   void tick(float *Storage, const double *Inject, double *Outputs, double &Peak) {
      double mixed[REVERB_LINES];
      for (int i = 0; i < REVERB_LINES; i++) {
         const double output = Storage[Offset[i] + Cursor[i]];
         Outputs[i] = output;
         State[i] = output + Pole[i] * (State[i] - output);
         mixed[i] = Gain[i] * State[i];
      }

      reverb_hadamard(mixed);

      for (int i = 0; i < REVERB_LINES; i++) {
         double value = mixed[i] + Inject[i];
         const double magnitude = std::abs(value);
         if (magnitude < 1e-30) value = 0; // Keep float storage and the absorption filters out of denormal range
         else Peak = std::max(Peak, magnitude);
         Storage[Offset[i] + Cursor[i]] = float(value);
         if (++Cursor[i] IS Length[i]) Cursor[i] = 0;
      }

      if (Ramp) {
         if (--Ramp) {
            for (int i = 0; i < REVERB_LINES; i++) {
               Gain[i] += GainStep[i];
               Pole[i] += PoleStep[i];
            }
            Norm += NormStep;
         }
         else {
            Gain = GainEnd;
            Pole = PoleEnd;
            Norm = NormEnd;
         }
      }
   }
};

//********************************************************************************************************************

class ReverbProcessor final : public AudioEffectProcessor {
public:
   extAudioEffect *Owner;
   ReverbSettings Settings; // Committed parameters, read by reset() under the mixer lock
   ReverbTarget Target;     // Derived from Settings for the current rate while active
   int Rate = 0;
   int Channels = 1;

private:
   // Storage prepared for storage_rate and storage_channels, swapped in by Configuration::publish().
   std::array<std::vector<float>, 2> line_storage;
   std::vector<float> predelay_storage, diffuser_storage;
   std::array<int, REVERB_LINES> line_offsets {};
   std::array<std::array<int, REVERB_DIFFUSERS>, 2> diffuser_offset {}, diffuser_length {}, diffuser_cursor {};
   int storage_rate = 0, storage_channels = 0, predelay_capacity = 0, line_capacity = 0;

   std::array<ReverbNetwork, 2> networks;
   ReverbTarget pending_target;
   int current = 0;                        // Index of the network receiving input; the other may be fading out
   int transition_left = 0, transition_frames = 1;
   int feed_left = 0;                      // Frames left in the input crossfade between networks
   bool has_pending_network = false;

   int predelay_cursor = 0, delay = 0, old_delay = 0, pending_delay = 0, delay_left = 0;
   bool has_pending_delay = false;

   double diffusion = 0, diffusion_end = 0, diffusion_step = 0;
   double mix = 0, mix_end = 0, mix_step = 0;
   int shared_ramp = 0, ramp_frames = 1;

   double reference = 0;   // Peak input magnitude since the last reset
   int quiet = 0;          // Consecutive frames in which every stored value was below the residual bound
   int memory = 0;         // Frames needed to overwrite every buffer
   bool active = false;
   bool dirty = true;      // Storage may hold non-zero values

   void start_network(const ReverbTarget &Next) {
      current ^= 1;
      networks[current].configure(line_storage[current].data(), line_offsets, Next, true);
      transition_left = transition_frames;
      feed_left = ramp_frames;
   }

   void start_delay(int Frames) {
      old_delay = delay;
      delay = Frames;
      delay_left = ramp_frames;
   }

   double read_delay(int Frames, int Channel) const {
      int index = predelay_cursor - Frames;
      if (index < 0) index += predelay_capacity;
      return predelay_storage[size_t(index) * Channels + Channel];
   }

   double diffuse(int Channel, double Input, double &Peak) {
      for (int stage = 0; stage < REVERB_DIFFUSERS; stage++) {
         auto &cursor = diffuser_cursor[Channel][stage];
         float &slot = diffuser_storage[diffuser_offset[Channel][stage] + cursor];
         const double delayed = slot;
         double value = Input + diffusion * delayed;
         const double magnitude = std::abs(value);
         if (magnitude < 1e-30) value = 0;
         else Peak = std::max(Peak, magnitude);
         Input = delayed - diffusion * value;
         slot = float(value);
         if (++cursor IS diffuser_length[Channel][stage]) cursor = 0;
      }
      return Input;
   }

   void clear() {
      for (auto &lines : line_storage) std::fill(lines.begin(), lines.end(), 0.0f);
      std::fill(predelay_storage.begin(), predelay_storage.end(), 0.0f);
      std::fill(diffuser_storage.begin(), diffuser_storage.end(), 0.0f);
      dirty = false;
   }

public:
   class Configuration final : public AudioEffectConfiguration {
   public:
      ReverbProcessor *Processor;
      std::array<std::vector<float>, 2> Lines;
      std::vector<float> PreDelay, Diffusers;
      std::array<int, REVERB_LINES> LineOffsets {};
      std::array<std::array<int, REVERB_DIFFUSERS>, 2> DiffuserOffset {}, DiffuserLength {};
      int Rate = 0, Channels = 0, PreDelayCapacity = 0, LineCapacity = 0;

      explicit Configuration(ReverbProcessor *Target) : Processor(Target) { }

      // Swap under the mixer lock.  The processor's previous storage is retired here and freed after unlocking.
      void publish() override {
         auto &p = *Processor;
         p.line_storage.swap(Lines);
         p.predelay_storage.swap(PreDelay);
         p.diffuser_storage.swap(Diffusers);
         p.line_offsets = LineOffsets;
         p.diffuser_offset = DiffuserOffset;
         p.diffuser_length = DiffuserLength;
         p.storage_rate = Rate;
         p.storage_channels = Channels;
         p.predelay_capacity = PreDelayCapacity;
         p.line_capacity = LineCapacity;
         p.active = false;
         p.dirty = false;
      }

      int64_t latency() const override { return 0; }
   };

   ReverbProcessor(extAudioEffect *Effect, const ReverbSettings &Initial) : Owner(Effect), Settings(Initial) { }

   // Storage depends only on the rate and channel count.  Both networks are sized for the largest room, so parameter
   // changes never allocate.  A zero rate leaves the processor inactive until the output is configured.

   ERR prepare(int PrepareRate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) override {
      auto config = std::make_unique<Configuration>(this);
      if (PrepareRate > 0) {
         if ((PrepareRate < REVERB_MIN_RATE) or (PrepareRate > REVERB_MAX_RATE)) return ERR::NoSupport;

         const int channels = Stereo ? 2 : 1;
         std::array<int, REVERB_LINES> capacity;
         reverb_lengths(100, PrepareRate, capacity);
         size_t total = 0;
         for (int i = 0; i < REVERB_LINES; i++) {
            config->LineOffsets[i] = int(total);
            total += size_t(capacity[i]);
            config->LineCapacity = std::max(config->LineCapacity, capacity[i]);
         }
         for (auto &lines : config->Lines) lines.assign(total, 0.0f);

         config->PreDelayCapacity = reverb_predelay_capacity(PrepareRate);
         config->PreDelay.assign(size_t(config->PreDelayCapacity) * channels, 0.0f);

         int diffusers = 0;
         for (int c = 0; c < channels; c++) {
            for (int stage = 0; stage < REVERB_DIFFUSERS; stage++) {
               config->DiffuserOffset[c][stage] = diffusers;
               config->DiffuserLength[c][stage] = reverb_diffuser_length(c, stage, PrepareRate);
               diffusers += config->DiffuserLength[c][stage];
            }
         }
         config->Diffusers.assign(size_t(diffusers), 0.0f);
         config->Rate = PrepareRate;
         config->Channels = channels;
      }
      Result = std::move(config);
      return ERR::Okay;
   }

   // Caller holds the mixer lock.  Next is derived on the control thread for the rate that was current at the time,
   // or is null if that rate is unsupported.  Ordinary edits ramp over 10 ms.  A size change starts a new network
   // while the previous one fades out; an edit during that transition replaces a single queued target.

   void update(const ReverbSettings &NextSettings, const ReverbTarget *Next) {
      Settings = NextSettings;
      if ((not active) or Owner->ResetPending or (not Next) or (Next->Rate != Rate)) return;

      Target = *Next;
      diffusion_end = Next->Diffusion;
      mix_end = Next->Mix;
      diffusion_step = (diffusion_end - diffusion) / double(ramp_frames);
      mix_step = (mix_end - mix) / double(ramp_frames);
      shared_ramp = ramp_frames;

      if (delay_left) {
         pending_delay = Next->PreDelay;
         has_pending_delay = pending_delay != delay;
      }
      else if (Next->PreDelay != delay) start_delay(Next->PreDelay);

      if (Next->Lengths IS networks[current].Length) {
         has_pending_network = false;
         networks[current].retarget(*Next, ramp_frames);
      }
      else if (transition_left) {
         pending_target = *Next;
         has_pending_network = true;
      }
      else start_network(*Next);
   }

   // The finite bound covers the buffers, the fading network and 180 dB of decay at DC.  Internal line levels can
   // exceed the input peak by several tens of decibels for long decays, so the residual bound of 120 dB below the
   // input peak is reached within this interval.

   AudioTail tail() const override { return AudioTail::FINITE; }

   uint64_t tail_frames() const override {
      if (not active) return 0;
      return uint64_t(memory) * 2 + uint64_t(transition_frames) + uint64_t(std::ceil(3.0 * Target.DecayFrames));
   }

   bool pending() const override {
      return active and ((transition_left > 0) or (quiet < memory));
   }

   // Pre-delay and the first pass through the longest line, then the 60 dB decay time.  Queued targets are included,
   // so the estimate covers the settings that will apply once current transitions finish.

   uint64_t decay_estimate() const override {
      if (not active) return 0;
      const int predelay = std::max(delay, has_pending_delay ? pending_delay : 0);
      int longest = 0;
      for (int i = 0; i < REVERB_LINES; i++) {
         longest = std::max(longest, networks[current].Length[i]);
         if (has_pending_network) longest = std::max(longest, pending_target.Lengths[i]);
      }
      return uint64_t(predelay) + uint64_t(longest) + uint64_t(std::ceil(Target.DecayFrames));
   }

   void reset() override {
      Rate = Owner->OutputRate;
      Channels = Owner->Stereo ? 2 : 1;
      has_pending_network = has_pending_delay = false;
      transition_left = feed_left = delay_left = shared_ramp = 0;
      reference = 0;

      if (dirty) clear();

      active = (storage_rate > 0) and (Rate IS storage_rate) and (Channels <= storage_channels) and
         (Owner->Layout.size() <= 2) and reverb_target(Settings, Rate, Target);
      if (not active) return;

      ramp_frames = std::max(1, Rate / 100);        // 10 ms
      transition_frames = std::max(1, Rate / 10);   // 100 ms
      networks[current].configure(line_storage[current].data(), line_offsets, Target, false);
      predelay_cursor = 0;
      delay = old_delay = Target.PreDelay;
      diffuser_cursor = {};
      diffusion = diffusion_end = Target.Diffusion;
      mix = mix_end = Target.Mix;
      memory = std::max(predelay_capacity, line_capacity);
      quiet = memory;
   }

   void process(float *Buffer, int Frames) override {
      if (not active) return;
      dirty = true;

      for (int frame = 0; frame < Frames; ++frame) {
         if ((not transition_left) and has_pending_network) {
            start_network(pending_target);
            has_pending_network = false;
         }

         if ((not delay_left) and has_pending_delay) {
            start_delay(pending_delay);
            has_pending_delay = false;
         }

         float *io = Buffer + size_t(frame) * Channels;
         double dry[2], diffused[2], peak = 0;
         const double head_blend = delay_left ? 1.0 - double(delay_left) / double(ramp_frames) : 1.0;

         for (int c = 0; c < Channels; c++) {
            dry[c] = io[c];
            const double magnitude = std::abs(dry[c]);
            reference = std::max(reference, magnitude);
            peak = std::max(peak, magnitude);
            predelay_storage[size_t(predelay_cursor) * Channels + c] = io[c];

            double delayed = read_delay(delay, c);
            if (delay_left) {
               const double old_head = read_delay(old_delay, c);
               delayed = old_head + (delayed - old_head) * head_blend;
            }
            diffused[c] = diffuse(c, delayed, peak);
         }

         if (++predelay_cursor IS predelay_capacity) predelay_cursor = 0;

         double inject[REVERB_LINES], outputs[REVERB_LINES];
         if (Channels IS 1) {
            for (int i = 0; i < REVERB_LINES; i++) inject[i] = diffused[0] * 0.35355339059327373; // 1 / sqrt(8)
         }
         else for (int i = 0; i < REVERB_LINES; i++) inject[i] = diffused[i & 1] * 0.5;

         // Taps use the coefficients that were current when the frame's line outputs were read.

         double wet[2] = { 0, 0 };
         auto tap = [&](double Gain) {
            if (Channels IS 1) {
               double sum = 0;
               for (int i = 0; i < REVERB_LINES; i++) sum += glReverbMonoTap[i] * outputs[i];
               wet[0] += sum * Gain;
            }
            else {
               double left = 0, right = 0;
               for (int i = 0; i < REVERB_LINES; i++) {
                  left += glReverbLeftTap[i] * outputs[i];
                  right += glReverbRightTap[i] * outputs[i];
               }
               wet[0] += left * Gain;
               wet[1] += right * Gain;
            }
         };

         // During a size transition the input crossfades from the previous network to the new one over 10 ms, so
         // neither network sees a step in its input.  The previous network's output fades out over 100 ms.

         const bool transitioning = transition_left > 0;
         double old_inject[REVERB_LINES];
         if (transitioning) {
            const double feed = double(feed_left) / double(ramp_frames);
            for (int i = 0; i < REVERB_LINES; i++) {
               old_inject[i] = inject[i] * feed;
               inject[i] -= old_inject[i];
            }
            if (feed_left) --feed_left;
         }

         auto &network = networks[current];
         const double norm = network.Norm;
         network.tick(line_storage[current].data(), inject, outputs, peak);
         tap(norm);

         if (transitioning) {
            const int previous = current ^ 1;
            auto &old_network = networks[previous];
            const double old_norm = old_network.Norm;
            old_network.tick(line_storage[previous].data(), old_inject, outputs, peak);
            tap(old_norm * double(transition_left) / double(transition_frames));
            --transition_left;
         }

         for (int c = 0; c < Channels; c++) io[c] = float(dry[c] + (wet[c] - dry[c]) * mix);

         if (shared_ramp) {
            if (--shared_ramp) {
               diffusion += diffusion_step;
               mix += mix_step;
            }
            else {
               diffusion = diffusion_end;
               mix = mix_end;
            }
         }

         if (delay_left) --delay_left;

         if (peak > std::max(reference * REVERB_RESIDUAL, 1e-30)) quiet = 0;
         else if (quiet < memory) ++quiet;
      }
   }
};
