#pragma once

#include <cmath>

// Constructed once during module loading, before any render worker exists. No lazy initialisation or
// mutable per-voice tables. Blackman-windowed sinc, 128 taps (-63..64), 256 phases plus the interpolation
// endpoint, and 65 uniformly spaced cutoffs. Each row has unity DC gain. Bilinear table interpolation
// keeps phase and ratio changes continuous within the downsampling range. Double accumulation preserves
// overshoot; clipping belongs to final output conversion. The centred kernel adds no output-frame delay.
struct SincTable {
   float Coefficients[glSincCutoffs + 1][glSincPhases + 1][glSincTaps];

   SincTable() {
      constexpr double pi = 3.14159265358979323846;
      for (int bank = 0; bank <= glSincCutoffs; ++bank) {
         const double cutoff = double(bank) / glSincCutoffs;
         for (int phase = 0; phase <= glSincPhases; ++phase) {
            double values[glSincTaps], sum = 0;
            for (int tap = 0; tap < glSincTaps; ++tap) {
               const double x = tap - glSincPast - double(phase) / glSincPhases;
               const double window = 0.42 + 0.5 * std::cos(pi * x / 64) +
                  0.08 * std::cos(2 * pi * x / 64);
               // The zero-cutoff bank is the normalised limiting window, not a division by zero.
               const double argument = pi * cutoff * x;
               values[tap] = window * (std::abs(argument) < 1e-12 ? 1 : std::sin(argument) / argument);
               sum += values[tap];
            }
            for (int tap = 0; tap < glSincTaps; ++tap) Coefficients[bank][phase][tap] = float(values[tap] / sum);
         }
      }
   }
};

static const SincTable glSincTable;

// Forward-started resident loops retain their real introduction until it leaves the kernel support.
// This is the same chronological continuation that the stream producer places in its ring. Reverse
// starts use periodic spatial extension, as do the older interpolation kernels.
static int64_t sinc_neighbour(int64_t Index, const MixingParams &Params)
{
   if (Params.loop_timeline and Params.loop_type != LTYPE::NIL and Params.loop_end > Params.loop_start) {
      int64_t time = Params.timeline_offset + (Params.timeline_reverse ? -Index : Index);
      if (time >= Params.loop_end) {
         const int64_t length = Params.loop_end - Params.loop_start;
         const int64_t period = Params.loop_type IS LTYPE::BIDIRECTIONAL ? 2 * length : length;
         int64_t offset = (time - Params.loop_start) % period;
         if (offset >= length) offset = period - 1 - offset;
         time = Params.loop_start + offset;
      }
      return std::clamp(time, int64_t(0), Params.length - 1) - Params.origin;
   }
   return resampler_neighbour(Index, Params);
}
