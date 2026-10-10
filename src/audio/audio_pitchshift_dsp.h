#pragma once

// Phase-vocoder pitch shifter for the AudioPitchShift processor.
//
// The algorithm is the peak-shifting phase vocoder of Laroche and Dolson ("New phase-vocoder techniques for
// pitch-shifting, harmonizing and other exotic effects", WASPAA 1999), linked across the stereo channels:
//
//   input -> N-frame Hann analysis every R = N/8 frames -> FFT -> peak shift -> IFFT -> Hann synthesis -> wet
//   input -> N-frame delay --------------------------------------------------------------------------------> dry
//   output = trim * ((1 - mix) * dry + mix * wet)
//
// N is the largest power of two not exceeding Rate/16: 2048 at 44.1 and 48 kHz, 256 at 8 kHz and 8192 at 192 kHz, so
// the window spans 32 to 47 ms at every supported rate.  The latency is exactly N output frames at every setting.
//
// Analysis.  Each frame is windowed and rotated by N/2 before the FFT, so that phases are measured at the frame centre.
// Both channels are transformed together, packed as the real and imaginary parts of one complex FFT.  The combined
// power P(k) = sum |X_c(k)|^2 selects the peaks: bins greater than both neighbours on each side and above
// PITCH_PEAK_FLOOR of the frame maximum.  Each peak owns the region of bins up to the lowest bin between it and the
// next peak.  The true frequency of a peak is estimated from the phase advance of the combined cross spectrum
// sum X_c(k) conj(X'_c(k)) against the previous frame X'.
//
// Shift.  For a ratio b, the region of the peak at bin p moves by the whole number of bins round((b - 1) f), where f
// is the estimated frequency of the peak in bins, and every bin of the region is multiplied by the same phasor
// g exp(j theta), where g is the makeup gain described below.  theta advances by (b - 1) w R per frame, where w R is
// the estimated phase advance of the peak per frame, and continues from the rotation of the previous frame's region
// that contained bin p.  Each partial therefore leaves with frequency b w exactly, while the moved region keeps the
// shape and the relative phases of the analysis, which limits phasiness and keeps transients compact.  Regions that
// move outside the spectrum are discarded; regions that overlap after a downward shift are summed.
//
// Makeup.  Rounding the move to whole bins leaves the frequency within each frame up to e = 1/2 bin from b w.
// Successive frames then meet with slightly different phases and partly cancel, so a sustained partial would lose up to
// 0.9 dB.  The loss depends only on e and the window, so each region is scaled by its reciprocal, tabulated in
// PitchTables::Makeup, which restores the level of sustained partials.  Noise-like content is also decorrelated
// between frames by the independent rotations of neighbouring regions, which no fixed gain can restore: it loses 0.5
// to 1.4 dB, growing with the shift.
//
// Stereo.  The peaks, regions and rotations are derived from both channels together and applied identically to each,
// so the level and phase relationship between the channels within a region is preserved exactly.  Mono runs one
// channel through the same path.
//
// Synthesis.  The spectrum is inverted, rotated back, windowed again and overlap-added with the constant gain
// R / sum(w^2), which is 1/3 for Hann at eight-fold overlap.  With no rotation the analysis-synthesis pair is an
// identity apart from rounding, so at zero semitones the wet signal is the input delayed by N frames.  When the ratio
// returns to one after a shift, each region's rotation steps back towards zero by at most PITCH_REALIGN_STEP per frame,
// briefly detuning partials by at most Rate / (32 R) Hz (6 Hz at 48 kHz) so that the wet path realigns with the dry
// path within two windows.
//
// Controls.  The ratio is read at each analysis frame, so a semitone change takes effect at the next frame and blends
// in over the overlapping windows.  Mix and trim ramp linearly over 10 ms.
//
// Tail.  An input frame contributes to the frames that end within N - 1 frames of it, and each of those frames is
// emitted over N frames after the latency, so non-zero output can follow the last non-zero input by at most 2N - 1
// frames.  Once that countdown expires every buffer is exactly zero and frames are skipped without evaluation.

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <vector>

constexpr int PITCH_MIN_RATE = 8000;
constexpr int PITCH_MAX_RATE = 192000;
constexpr int PITCH_OVERLAP = 8;             // Analysis frames per window
constexpr int PITCH_MIN_WINDOW = 256;
constexpr int PITCH_MAX_WINDOW = 8192;
constexpr double PITCH_MAX_SHIFT = 12;       // Semitones in either direction; matches the published range
constexpr double PITCH_PEAK_FLOOR = 1e-14;   // Peaks below this proportion of the frame's peak power are ignored
constexpr double PITCH_REALIGN_STEP = std::numbers::pi / 16; // Largest rotation change per frame at unity ratio
constexpr double PITCH_SILENCE = 1e-30;      // Input magnitudes below this are treated as silence
constexpr int PITCH_MAKEUP_STEPS = 256;      // Table resolution for in-frame frequency errors from 0 to 1/2 bin
constexpr double PITCH_DB_TO_LOG = std::numbers::ln10 / 20.0;

struct PitchShiftSettings {
   double Semitones = 0;
   double Mix = 100;          // Percent
   double Gain = 0;           // dB
};

// DSP-ready values derived from PitchShiftSettings for one sample rate.  Deriving them performs no allocation.

struct PitchShiftTarget {
   int SampleRate = 0;
   double Ratio = 1;          // Frequency ratio
   double Mix = 1;            // Wet proportion, 0 to 1
   double Trim = 1;           // Linear output gain
};

//********************************************************************************************************************
// Window length in frames: the largest power of two not exceeding Rate/16, which is also the latency.

inline int pitch_window(int Rate)
{
   return std::clamp(int(std::bit_floor(unsigned(std::max(Rate / 16, 1)))), PITCH_MIN_WINDOW, PITCH_MAX_WINDOW);
}

inline double pitch_ratio(double Semitones)
{
   return std::exp2(std::clamp(Semitones, -PITCH_MAX_SHIFT, PITCH_MAX_SHIFT) / 12.0);
}

// Wrap a phase to [-pi, pi).

inline double pitch_wrap(double Phase)
{
   constexpr double tau = 2.0 * std::numbers::pi;
   return Phase - tau * std::floor((Phase + std::numbers::pi) / tau);
}

//********************************************************************************************************************
// Derive the complete processing target.  Returns false for unsupported rates.

inline bool pitch_target(const PitchShiftSettings &Settings, int Rate, PitchShiftTarget &Target)
{
   if ((Rate < PITCH_MIN_RATE) or (Rate > PITCH_MAX_RATE)) return false;
   Target.SampleRate = Rate;
   Target.Ratio      = pitch_ratio(Settings.Semitones);
   Target.Mix        = std::clamp(Settings.Mix, 0.0, 100.0) / 100.0;
   Target.Trim       = std::exp(Settings.Gain * PITCH_DB_TO_LOG);
   return true;
}

//********************************************************************************************************************
// Fixed tables for one window length, built outside the mixer lock.  Complex values are stored as interleaved
// real and imaginary doubles throughout, which keeps the transforms free of library calls.

struct PitchTables {
   int Size = 0;
   std::vector<double> Window;     // Periodic Hann
   std::vector<double> Twiddles;   // exp(-2 pi j k / len) for k < len / 2, for each stage len = 2, 4, ... Size
   std::vector<int> Reverse;       // Bit-reversed index permutation
   std::vector<double> Makeup;     // Gain for an in-frame frequency error of |e| bins, e = 0 to 1/2 inclusive
   double Norm = 1;                // Overlap-add gain, R / sum(w^2)

   explicit PitchTables(int N = 0) : Size(N) {
      if (N <= 0) return;
      Window.resize(size_t(N));
      double energy = 0;
      for (int i = 0; i < N; i++) {
         Window[size_t(i)] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(N));
         energy += Window[size_t(i)] * Window[size_t(i)];
      }
      Norm = double(N / PITCH_OVERLAP) / energy;

      Twiddles.reserve(size_t(2 * N));
      for (int len = 2; len <= N; len <<= 1) {
         for (int k = 0; k < len / 2; k++) {
            const double angle = -2.0 * std::numbers::pi * double(k) / double(len);
            Twiddles.push_back(std::cos(angle));
            Twiddles.push_back(std::sin(angle));
         }
      }

      // A partial that leaves each frame e bins from its true frequency drifts in phase by 2 pi e (t - c) / N across
      // the frame centred at c, so the overlapping frames sum to G(e) of its level.  G is averaged over the R output
      // positions between frame starts, and the makeup is its reciprocal.

      const int hop = N / PITCH_OVERLAP;
      Makeup.resize(PITCH_MAKEUP_STEPS + 1);
      for (int step = 0; step <= PITCH_MAKEUP_STEPS; step++) {
         const double e = 0.5 * double(step) / double(PITCH_MAKEUP_STEPS);
         double sum = 0;
         for (int t = 0; t < hop; t++) {
            double re = 0, im = 0;
            for (int m = 0; m < PITCH_OVERLAP; m++) {
               const int i = t + m * hop;
               const double weight = Window[size_t(i)] * Window[size_t(i)];
               const double angle = 2.0 * std::numbers::pi * e * double(i - N / 2) / double(N);
               re += weight * std::cos(angle);
               im += weight * std::sin(angle);
            }
            sum += std::sqrt(re * re + im * im) * Norm;
         }
         Makeup[size_t(step)] = double(hop) / sum;
      }
      Makeup[0] = 1.0; // Exact by construction; avoids rounding in the sum

      Reverse.resize(size_t(N));
      const int bits = std::countr_zero(unsigned(N));
      for (int i = 0; i < N; i++) {
         unsigned r = 0;
         for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1u << (bits - 1 - b);
         Reverse[size_t(i)] = int(r);
      }
   }

   // In-place radix-2 FFT of Size interleaved complex values.  The inverse is unscaled.

   // Makeup gain for an in-frame frequency error of Error bins, interpolated from the table.

   double makeup(double Error) const {
      const double position = std::min(std::abs(Error), 0.5) * double(2 * PITCH_MAKEUP_STEPS);
      const int index = std::min(int(position), PITCH_MAKEUP_STEPS - 1);
      const double fraction = position - double(index);
      return Makeup[size_t(index)] + (Makeup[size_t(index + 1)] - Makeup[size_t(index)]) * fraction;
   }

   void fft(double *Data, bool Inverse) const {
      const int n = Size;
      for (int i = 0; i < n; i++) {
         const int j = Reverse[size_t(i)];
         if (i < j) {
            std::swap(Data[2 * i], Data[2 * j]);
            std::swap(Data[2 * i + 1], Data[2 * j + 1]);
         }
      }

      const double sign = Inverse ? -1.0 : 1.0;
      const double *stage = Twiddles.data();
      for (int len = 2; len <= n; len <<= 1) {
         const int half = len >> 1;
         for (int i = 0; i < n; i += len) {
            double *a = Data + 2 * i, *b = a + 2 * half;
            for (int k = 0; k < half; k++) {
               const double wr = stage[2 * k], wi = sign * stage[2 * k + 1];
               const double br = b[2 * k], bi = b[2 * k + 1];
               const double vr = br * wr - bi * wi, vi = br * wi + bi * wr;
               const double ar = a[2 * k], ai = a[2 * k + 1];
               a[2 * k] = ar + vr;
               a[2 * k + 1] = ai + vi;
               b[2 * k] = ar - vr;
               b[2 * k + 1] = ai - vi;
            }
         }
         stage += 2 * half;
      }
   }
};

//********************************************************************************************************************

class PitchShiftProcessor final : public AudioEffectProcessor {
public:
   // A value that ramps linearly to End over the remaining frames of the shared ramp.

   struct Ramp {
      double Value = 0, End = 0, Step = 0;

      void snap(double Target) { Value = End = Target; Step = 0; }
      void start(double Target, int Frames) { End = Target; Step = (End - Value) / double(Frames); }
      void advance(bool Last) { Value = Last ? End : Value + Step; }
   };

   extAudioEffect *Owner;
   PitchShiftSettings Settings; // Committed parameters, read by reset() under the mixer lock
   PitchShiftTarget Target;     // Derived from Settings for the current rate while active
   int SampleRate = 0;
   int Channels = 1;

private:
   // Storage prepared for storage_rate and storage_channels, swapped in by Configuration::publish().  Spectra hold
   // N/2 + 1 interleaved complex bins per channel.
   PitchTables tables;
   std::vector<double> input;    // N per channel: the analysis frame, filled from N - R onwards
   std::vector<double> accum;    // N per channel: overlap-add accumulator aligned with the input frame
   std::vector<double> emit;     // R per channel: completed output for the current hop
   std::vector<float> dry;       // N per channel: the latency-aligned dry signal
   std::vector<double> buffer;   // N complex: packed FFT workspace
   std::vector<double> spectrum, previous, shifted; // Current analysis, previous analysis and synthesis
   std::vector<double> power;    // N/2 + 1
   std::vector<double> rotation, next_rotation; // N/2 + 1: rotation applied to each analysis bin's region
   std::vector<int> peaks;       // Up to N/4 + 1
   int storage_rate = 0, storage_channels = 0;

   int size = 0, hop = 0, bins = 0; // N, R and N/2
   int fill = 0;                 // Frames of the current hop received
   int dry_pos = 0;              // Slot of the oldest dry frame, which is replaced by the next input
   int countdown = 0;            // Frames after the current one for which non-zero output may remain
   double ratio = 1;
   Ramp mix, trim;
   int ramp_left = 0, ramp_frames = 1;
   bool active = false;

   void clear() {
      std::fill(input.begin(), input.end(), 0.0);
      std::fill(accum.begin(), accum.end(), 0.0);
      std::fill(emit.begin(), emit.end(), 0.0);
      std::fill(dry.begin(), dry.end(), 0.0f);
      std::fill(previous.begin(), previous.end(), 0.0);
      std::fill(rotation.begin(), rotation.end(), 0.0);
      fill = dry_pos = countdown = 0;
   }

   void step_ramps() {
      if (not ramp_left) return;
      --ramp_left;
      mix.advance(not ramp_left);
      trim.advance(not ramp_left);
   }

   // Analyse the current frame of every channel into spectrum.  Two channels share one transform as its real and
   // imaginary parts, and are separated by the conjugate symmetry of real signals.

   void analyse() {
      const int n = size, k_max = bins, stride = 2 * (k_max + 1);
      const double *w = tables.Window.data();
      double *z = buffer.data();
      for (int pair = 0; pair < Channels; pair += 2) {
         const double *left = input.data() + size_t(pair) * n;
         const double *right = (pair + 1 < Channels) ? left + n : nullptr;
         for (int i = 0; i < n; i++) {
            const int j = (i + n / 2) & (n - 1);
            z[2 * j] = left[i] * w[i];
            z[2 * j + 1] = right ? right[i] * w[i] : 0.0;
         }
         tables.fft(z, false);

         double *xl = spectrum.data() + size_t(pair) * stride;
         if (right) {
            double *xr = xl + stride;
            for (int k = 0; k <= k_max; k++) {
               const int m = (n - k) & (n - 1);
               const double ar = z[2 * k], ai = z[2 * k + 1], br = z[2 * m], bi = z[2 * m + 1];
               xl[2 * k] = 0.5 * (ar + br);
               xl[2 * k + 1] = 0.5 * (ai - bi);
               xr[2 * k] = 0.5 * (ai + bi);
               xr[2 * k + 1] = 0.5 * (br - ar);
            }
         }
         else std::copy(z, z + stride, xl);
      }
   }

   // Move the peak regions of spectrum into shifted, applying one rotation per region to every channel.

   void shift() {
      const int k_max = bins, stride = 2 * (k_max + 1);
      std::fill(shifted.begin(), shifted.end(), 0.0);

      double top = 0;
      double *pw = power.data();
      for (int k = 0; k <= k_max; k++) {
         double p = 0;
         for (int c = 0; c < Channels; c++) {
            const double *x = spectrum.data() + size_t(c) * stride + 2 * k;
            p += x[0] * x[0] + x[1] * x[1];
         }
         pw[k] = p;
         if (p > top) top = p;
      }

      int count = 0;
      if (top > 0) {
         const double floor = top * PITCH_PEAK_FLOOR;
         // The leftmost bin owns a plateau.  Include the spectrum edges so that DC, Nyquist and flat spectra such as
         // an impulse always have at least one region owner.
         if ((pw[0] > floor) and (pw[0] >= pw[1]) and (pw[0] >= pw[2])) peaks[size_t(count++)] = 0;
         for (int k = 1; k < k_max; k++) {
            const double p = pw[k];
            if ((p <= floor) or (p <= pw[k - 1]) or (p < pw[k + 1])) continue;
            if ((k >= 2) and (p <= pw[k - 2])) continue;
            if ((k + 2 <= k_max) and (p < pw[k + 2])) continue;
            peaks[size_t(count++)] = k;
         }
         if ((pw[k_max] > floor) and (pw[k_max] > pw[k_max - 1]) and (pw[k_max] > pw[k_max - 2])) {
            peaks[size_t(count++)] = k_max;
         }
      }

      if (not count) {
         std::fill(rotation.begin(), rotation.end(), 0.0);
         return;
      }

      const double tau = 2.0 * std::numbers::pi;
      int start = 0;
      for (int i = 0; i < count; i++) {
         const int p = peaks[size_t(i)];
         int end = k_max;
         if (i + 1 < count) {
            const int q = peaks[size_t(i + 1)];
            end = p + 1;
            for (int k = p + 2; k < q; k++) if (pw[k] < pw[end]) end = k;
         }

         // The phase advance of the combined cross spectrum gives the true frequency in bins.
         double theta, frequency = double(p);
         if (ratio IS 1.0) {
            const double old = rotation[size_t(p)];
            theta = old - std::clamp(old, -PITCH_REALIGN_STEP, PITCH_REALIGN_STEP);
         }
         else {
            double cr = 0, ci = 0;
            for (int c = 0; c < Channels; c++) {
               const double *x = spectrum.data() + size_t(c) * stride + 2 * p;
               const double *y = previous.data() + size_t(c) * stride + 2 * p;
               cr += x[0] * y[0] + x[1] * y[1];
               ci += x[1] * y[0] - x[0] * y[1];
            }
            const double expected = tau * double(p) / double(PITCH_OVERLAP);
            double advance = expected;
            if ((cr != 0) or (ci != 0)) advance += pitch_wrap(std::atan2(ci, cr) - expected);
            frequency = advance * double(PITCH_OVERLAP) / tau;
            theta = pitch_wrap(rotation[size_t(p)] + (ratio - 1.0) * advance);
         }

         const double move = (ratio - 1.0) * frequency;
         const int offset = int(std::lround(move));
         const int from = std::max(start, -offset), to = std::min(end, k_max - offset);
         std::fill(next_rotation.begin() + start, next_rotation.begin() + end + 1, theta);
         const double gain = (ratio IS 1.0) ? 1.0 : tables.makeup(move - double(offset));
         const double qr = gain * std::cos(theta), qi = gain * std::sin(theta);
         for (int c = 0; c < Channels; c++) {
            const double *x = spectrum.data() + size_t(c) * stride;
            double *y = shifted.data() + size_t(c) * stride;
            if ((theta IS 0.0) and (gain IS 1.0)) {
               for (int k = 2 * from; k <= 2 * to + 1; k++) y[k + 2 * offset] += x[k];
            }
            else {
               for (int k = from; k <= to; k++) {
                  const double xr = x[2 * k], xi = x[2 * k + 1];
                  y[2 * (k + offset)] += xr * qr - xi * qi;
                  y[2 * (k + offset) + 1] += xr * qi + xi * qr;
               }
            }
         }
         start = end + 1;
      }
      rotation.swap(next_rotation);
   }

   // Invert shifted and overlap-add it into accum.  DC and Nyquist are kept real so that the packed channels stay
   // separate.

   void synthesise() {
      const int n = size, k_max = bins, stride = 2 * (k_max + 1);
      const double *w = tables.Window.data();
      const double scale = tables.Norm / double(n);
      double *z = buffer.data();
      for (int pair = 0; pair < Channels; pair += 2) {
         const double *yl = shifted.data() + size_t(pair) * stride;
         const double *yr = (pair + 1 < Channels) ? yl + stride : nullptr;

         // Z = Yl + j Yr for bins 0 to N/2, and conj(Yl) + j conj(Yr) for the mirrored bins.
         for (int k = 0; k <= k_max; k++) {
            const double lr = yl[2 * k], li = ((k IS 0) or (k IS k_max)) ? 0.0 : yl[2 * k + 1];
            double rr = 0, ri = 0;
            if (yr) {
               rr = yr[2 * k];
               ri = ((k IS 0) or (k IS k_max)) ? 0.0 : yr[2 * k + 1];
            }
            z[2 * k] = lr - ri;
            z[2 * k + 1] = li + rr;
            if ((k > 0) and (k < k_max)) {
               z[2 * (n - k)] = lr + ri;
               z[2 * (n - k) + 1] = rr - li;
            }
         }
         tables.fft(z, true);

         double *left = accum.data() + size_t(pair) * n;
         double *right = yr ? left + n : nullptr;
         for (int i = 0; i < n; i++) {
            const int j = (i + n / 2) & (n - 1);
            const double g = w[i] * scale;
            left[i] += z[2 * j] * g;
            if (right) right[i] += z[2 * j + 1] * g;
         }
      }
   }

   // Called when a hop of input is complete.  An all-zero frame is skipped: its analysis, and therefore its
   // contribution, is exactly zero.

   void hop_frame(bool Live) {
      const int n = size, r = hop;
      if (Live) {
         analyse();
         shift();
         synthesise();
         previous.swap(spectrum);
      }
      else {
         std::fill(previous.begin(), previous.end(), 0.0);
         std::fill(rotation.begin(), rotation.end(), 0.0);
      }

      for (int c = 0; c < Channels; c++) {
         double *a = accum.data() + size_t(c) * n;
         double *in = input.data() + size_t(c) * n;
         std::copy(a, a + r, emit.begin() + size_t(c) * r);
         std::copy(a + r, a + n, a);
         std::fill(a + n - r, a + n, 0.0);
         std::copy(in + r, in + n, in);
      }
   }

public:
   class Configuration final : public AudioEffectConfiguration {
   public:
      PitchShiftProcessor *Processor;
      PitchTables Tables;
      std::vector<double> Input, Accum, Emit, Buffer, Spectrum, Previous, Shifted, Power, Rotation, NextRotation;
      std::vector<float> Dry;
      std::vector<int> Peaks;
      int Rate = 0, Channels = 0;

      explicit Configuration(PitchShiftProcessor *Target) : Processor(Target) { }

      // Swap under the mixer lock.  The processor's previous storage is retired here and freed after unlocking.
      void publish() override {
         auto &p = *Processor;
         std::swap(p.tables, Tables);
         p.input.swap(Input);
         p.accum.swap(Accum);
         p.emit.swap(Emit);
         p.dry.swap(Dry);
         p.buffer.swap(Buffer);
         p.spectrum.swap(Spectrum);
         p.previous.swap(Previous);
         p.shifted.swap(Shifted);
         p.power.swap(Power);
         p.rotation.swap(Rotation);
         p.next_rotation.swap(NextRotation);
         p.peaks.swap(Peaks);
         p.storage_rate = Rate;
         p.storage_channels = Channels;
         p.active = false;
         p.countdown = 0;
      }

      int64_t latency() const override { return (Rate > 0) ? pitch_window(Rate) : 0; }
   };

   PitchShiftProcessor(extAudioEffect *Effect, const PitchShiftSettings &Initial) : Owner(Effect), Settings(Initial) { }

   // Storage depends on the rate, through the window length, and on the channel count.  A zero rate leaves the
   // processor inactive, with no latency, until the output is configured.

   ERR prepare(int PrepareRate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) override {
      auto config = std::make_unique<Configuration>(this);
      if (PrepareRate > 0) {
         if ((PrepareRate < PITCH_MIN_RATE) or (PrepareRate > PITCH_MAX_RATE)) return ERR::NoSupport;
         const int n = pitch_window(PrepareRate), c = Stereo ? 2 : 1;
         const size_t spectral = size_t(n / 2 + 1);
         config->Rate = PrepareRate;
         config->Channels = c;
         config->Tables = PitchTables(n);
         config->Input.assign(size_t(n) * c, 0.0);
         config->Accum.assign(size_t(n) * c, 0.0);
         config->Emit.assign(size_t(n / PITCH_OVERLAP) * c, 0.0);
         config->Dry.assign(size_t(n) * c, 0.0f);
         config->Buffer.assign(size_t(2 * n), 0.0);
         config->Spectrum.assign(2 * spectral * c, 0.0);
         config->Previous.assign(2 * spectral * c, 0.0);
         config->Shifted.assign(2 * spectral * c, 0.0);
         config->Power.assign(spectral, 0.0);
         config->Rotation.assign(spectral, 0.0);
         config->NextRotation.assign(spectral, 0.0);
         config->Peaks.assign(spectral / 2 + 1, 0);
      }
      Result = std::move(config);
      return ERR::Okay;
   }

   // Caller holds the mixer lock.  Next is derived on the control thread for the rate that was current at the time,
   // or is null if that rate is unsupported.  The ratio applies from the next analysis frame; mix and trim ramp over
   // 10 ms from their current values.  All history is retained.

   void update(const PitchShiftSettings &NextSettings, const PitchShiftTarget *Next) {
      Settings = NextSettings;
      if ((not active) or Owner->ResetPending or (not Next) or (Next->SampleRate != SampleRate)) return;

      Target = *Next;
      ratio = Next->Ratio;
      mix.start(Next->Mix, ramp_frames);
      trim.start(Next->Trim, ramp_frames);
      ramp_left = ramp_frames;
   }

   // The bounds are independent of the settings, so that a later mix edit can never reveal output after the processor
   // has stopped reporting it.  The decay estimate covers the spread of a transient beyond the latency.

   AudioTail tail() const override { return AudioTail::FINITE; }
   uint64_t tail_frames() const override { return active ? uint64_t(2 * size) : 0; }
   uint64_t decay_estimate() const override { return active ? uint64_t(size * 3 / 4) : 0; }
   int64_t latency() const override { return (storage_rate > 0) ? pitch_window(storage_rate) : 0; }
   bool pending() const override { return active and (countdown > 0); }

   int window_size() const { return size; }
   int hop_size() const { return hop; }
   double frequency_ratio() const { return ratio; }
   double wet_mix() const { return mix.Value; }
   double trim_gain() const { return trim.Value; }
   bool ramping() const { return ramp_left > 0; }
   bool realigned() const {
      return std::all_of(rotation.begin(), rotation.end(), [](double Value) { return Value IS 0.0; });
   }
   size_t storage_bytes() const {
      return (input.size() + accum.size() + emit.size() + buffer.size() + spectrum.size() + previous.size() +
         shifted.size() + power.size() + rotation.size() + next_rotation.size() + tables.Window.size() +
         tables.Twiddles.size() + tables.Makeup.size()) * sizeof(double) + dry.size() * sizeof(float) +
         (peaks.size() + tables.Reverse.size()) * sizeof(int);
   }

   void reset() override {
      SampleRate = Owner->OutputRate;
      Channels = Owner->Stereo ? 2 : 1;

      active = (storage_rate > 0) and (SampleRate IS storage_rate) and (Channels <= storage_channels) and
         (Owner->Layout.size() <= 2) and pitch_target(Settings, SampleRate, Target);
      if (not active) return;

      size = tables.Size;
      hop = size / PITCH_OVERLAP;
      bins = size / 2;
      clear();
      ramp_frames = std::max(1, SampleRate / 100); // 10 ms
      ramp_left = 0;
      ratio = Target.Ratio;
      mix.snap(Target.Mix);
      trim.snap(Target.Trim);
   }

   // Non-finite input is outside the normalised pipeline contract and, like magnitudes below PITCH_SILENCE, is treated
   // as silence on entry.  While the countdown is zero every buffer is zero, so silent frames produce silence without
   // evaluating the transforms.  The hop cadence and ramps still advance, so output is independent of the partition.

   void process(float *Buffer, int Frames) override {
      if (not active) return;

      const int n = size, r = hop;
      for (int frame = 0; frame < Frames; ++frame) {
         float *io = Buffer + size_t(frame) * Channels;

         double x[2];
         bool input_set = false;
         for (int c = 0; c < Channels; c++) {
            x[c] = double(io[c]);
            if ((not std::isfinite(x[c])) or (std::abs(x[c]) < PITCH_SILENCE)) x[c] = 0;
            else input_set = true;
         }

         const bool live = input_set or (countdown > 0);
         if (input_set) countdown = 2 * n - 1;
         else if (countdown > 0) countdown--;

         if (live) {
            const double m = mix.Value, t = trim.Value;
            for (int c = 0; c < Channels; c++) {
               const double wet = emit[size_t(c * r + fill)];
               input[size_t(c * n + n - r + fill)] = x[c];
               float &slot = dry[size_t(c * n + dry_pos)];
               const double delayed = slot;
               slot = float(x[c]);
               io[c] = float(t * ((1.0 - m) * delayed + m * wet));
            }
         }
         else {
            // Idle: every buffer is zero and remains so, so slot contents are immaterial.
            for (int c = 0; c < Channels; c++) io[c] = 0.0f;
         }

         if (++dry_pos IS n) dry_pos = 0;
         step_ramps();
         if (++fill IS r) {
            fill = 0;
            hop_frame(live);
         }
      }
   }
};
