#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

// SplitMix64 streams have independently mixed channel seeds.  No allocation, locks or global random state.
// Exact silence suppresses noise immediately, but still advances the stream; gating never repeats a start.
struct AudioDitherState {
   std::array<uint64_t, 64> State{};
   unsigned Channel = 0;

   AudioDitherState() { reset(); }

   void reset(uint64_t Seed = 0x243f6a8885a308d3ULL) {
      for (std::size_t i = 0; i < State.size(); ++i) State[i] = scramble(Seed + i);
      Channel = 0;
   }

   static uint64_t scramble(uint64_t Value) {
      Value = (Value ^ (Value >> 30)) * 0xbf58476d1ce4e5b9ULL;
      Value = (Value ^ (Value >> 27)) * 0x94d049bb133111ebULL;
      return Value ^ (Value >> 31);
   }

   double uniform(unsigned Index) {
      State[Index] += 0x9e3779b97f4a7c15ULL;
      return double(scramble(State[Index]) >> 11) * 0x1p-53;
   }

   // Difference of two U[0,1) draws: triangular noise in (-1,1) destination LSB, variance 1/6.
   double next(unsigned Channels) {
      const double first = uniform(Channel);
      const double noise = first - uniform(Channel);
      if (++Channel >= Channels) Channel = 0;
      return noise;
   }
};
