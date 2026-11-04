#pragma once

// Independent complex sinusoid response of the prepared FIR; includes fractional phase and cutoff interpolation.
struct SincResponse {
   double RippleDb = 0;
   double StopDb = 0;
   double AliasRatio = 0;
};

static SincResponse measure_sinc(double Ratio)
{
   constexpr double pi = 3.14159265358979323846;
   const double bank = (Ratio <= 1 ? 1 : std::max(0.0, 1.0 / Ratio - 0.05)) * glSincCutoffs;
   const int lower = std::min(int(bank), glSincCutoffs - 1);
   const double blend = bank - lower;
   double peak = 0, alias = 0, cubic_alias = 0;
   SincResponse result;
   for (int phase = 0; phase < 16; ++phase) {
      const double fraction = (phase + 0.37) / 16;
      const double row_position = fraction * glSincPhases;
      const int row = int(row_position);
      const double subphase = row_position - row;
      for (int frequency = 1; frequency <= 2048; ++frequency) {
         const double f = 0.5 * frequency / 2048;
         double real = 0, imaginary = 0;
         for (int tap = 0; tap < glSincTaps; ++tap) {
            const auto &a = glSincTable.Coefficients[lower];
            const auto &b = glSincTable.Coefficients[lower + 1];
            const double first = a[row][tap] + subphase * (a[row + 1][tap] - a[row][tap]);
            const double second = b[row][tap] + subphase * (b[row + 1][tap] - b[row][tap]);
            const double coefficient = first + blend * (second - first);
            real += coefficient * std::cos(2 * pi * f * tap);
            imaginary += coefficient * std::sin(2 * pi * f * tap);
         }
         const double power = real * real + imaginary * imaginary;
         if (f <= 0.75 * 0.5 / std::max(1.0, Ratio)) {
            result.RippleDb = std::max(result.RippleDb, std::abs(10 * std::log10(power)));
         }
         if (Ratio > 1 and f >= 0.5 / Ratio) {
            peak = std::max(peak, power);
            alias += power;
            // Include the bounded cubic's nonlinearity by averaging sinusoidal start phases.
            for (int start = 0; start < 8; ++start) {
               auto wave = [&](int Tap) { return std::sin(2 * pi * (f * Tap + start / 8.0)); };
               const double value = resampler_cubic(wave(-1), wave(0), wave(1), wave(2), fraction);
               cubic_alias += value * value / 4;
            }
         }
      }
   }
   result.StopDb = peak > 0 ? -10 * std::log10(peak) : 0;
   result.AliasRatio = cubic_alias > 0 ? alias / cubic_alias : 0;
   return result;
}
