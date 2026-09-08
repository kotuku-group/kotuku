
#include <immintrin.h>  // For SIMD support where available

// Thread-local step value for mixing (temporary solution)
thread_local int MixStep = 1;

// Function to set the mixing step (called from functions.cpp)
void set_mix_step(int step) {
   MixStep = step;
}

template<typename T>
struct SampleTraits;

template<>
struct SampleTraits<uint8_t> {
   static constexpr double SCALE = 256.0;
   static constexpr double OFFSET = 128.0;

   static inline double normalize(uint8_t value) {
      return SCALE * (value - OFFSET);
   }
};

template<>
struct SampleTraits<int16_t> {
   static constexpr double SCALE = 1.0;
   static constexpr double OFFSET = 0.0;
   static constexpr double MAX_VALUE = 32767.0;

   static inline double normalize(int16_t value) {
      return double(value);
   }
};

//********************************************************************************************************************
// Optimized interpolation helper

struct InterpolationWeights {
   double weight0, weight1;

   inline InterpolationWeights(int fracPos) {
      weight0 = double(65536 - (fracPos & 0xFFFF));
      weight1 = double(fracPos & 0xFFFF);
   }
};

//********************************************************************************************************************
// Template-based mixer core - handles all sample types and channel configurations
// Interpolation is used when the Audio object has over-sampling enabled.

template<typename SampleType, bool IsStereoSample, bool IsStereoOutput, bool UseInterpolation>
static int mix_template(APTR Src, int SrcPos, int TotalSamples, int nextSampleOffset, float LeftVol, float RightVol, float **MixDest)
{
   auto dest = *MixDest;
   auto sample = (SampleType *)Src;
   constexpr int srcChannels = IsStereoSample ? 2 : 1;

   if constexpr (UseInterpolation) {
      // Interpolated mixing
      const bool useNextOffset = (nextSampleOffset != 1);

      while (TotalSamples > 0) {
         const int baseIdx = (SrcPos >> 16) * srcChannels;
         const int nextIdx = baseIdx + (useNextOffset ? nextSampleOffset * srcChannels : srcChannels);

         InterpolationWeights weights(SrcPos);

         if constexpr (IsStereoSample and IsStereoOutput) {
            // Stereo sample to stereo output with interpolation
            const double leftSample = (weights.weight0 * SampleTraits<SampleType>::normalize(sample[baseIdx]) +
                                      weights.weight1 * SampleTraits<SampleType>::normalize(sample[nextIdx])) / 65536.0;
            const double rightSample = (weights.weight0 * SampleTraits<SampleType>::normalize(sample[baseIdx + 1]) +
                                       weights.weight1 * SampleTraits<SampleType>::normalize(sample[nextIdx + 1])) / 65536.0;

            dest[0] += LeftVol * leftSample;
            dest[1] += RightVol * rightSample;
            dest += 2;

         }
         else if constexpr (IsStereoSample and !IsStereoOutput) {
            // Stereo sample to mono output with interpolation
            const double leftSample = (weights.weight0 * SampleTraits<SampleType>::normalize(sample[baseIdx]) +
                                      weights.weight1 * SampleTraits<SampleType>::normalize(sample[nextIdx])) / 65536.0;
            const double rightSample = (weights.weight0 * SampleTraits<SampleType>::normalize(sample[baseIdx + 1]) +
                                       weights.weight1 * SampleTraits<SampleType>::normalize(sample[nextIdx + 1])) / 65536.0;

            *(dest++) += LeftVol * (leftSample + rightSample);

         }
         else if constexpr (!IsStereoSample and IsStereoOutput) {
            // Mono sample to stereo output with interpolation
            const double monoSample = (weights.weight0 * SampleTraits<SampleType>::normalize(sample[baseIdx]) +
                                      weights.weight1 * SampleTraits<SampleType>::normalize(sample[nextIdx])) / 65536.0;

            dest[0] += LeftVol * monoSample;
            dest[1] += RightVol * monoSample;
            dest += 2;

         } else {
            // Mono sample to mono output with interpolation
            const double monoSample = (weights.weight0 * SampleTraits<SampleType>::normalize(sample[baseIdx]) +
                                      weights.weight1 * SampleTraits<SampleType>::normalize(sample[nextIdx])) / 65536.0;

            *(dest++) += LeftVol * monoSample;
         }

         SrcPos += MixStep;
         TotalSamples--;
      }
   }
   else { // Non-interpolated mixing - optimized for speed
      while (TotalSamples > 0) {
         const int baseIdx = (SrcPos >> 16) * srcChannels;

         if constexpr (IsStereoSample and IsStereoOutput) { // Stereo sample to stereo output
            dest[0] += LeftVol * SampleTraits<SampleType>::normalize(sample[baseIdx]);
            dest[1] += RightVol * SampleTraits<SampleType>::normalize(sample[baseIdx + 1]);
            dest += 2;

         }
         else if constexpr (IsStereoSample and !IsStereoOutput) { // Stereo sample to mono output
            const double combined = SampleTraits<SampleType>::normalize(sample[baseIdx]) +
                                   SampleTraits<SampleType>::normalize(sample[baseIdx + 1]);
            *(dest++) += LeftVol * combined;

         }
         else if constexpr (!IsStereoSample and IsStereoOutput) { // Mono sample to stereo output
            const double monoSample = SampleTraits<SampleType>::normalize(sample[baseIdx]);
            dest[0] += LeftVol * monoSample;
            dest[1] += RightVol * monoSample;
            dest += 2;

         }
         else { // Mono sample to mono output
            *(dest++) += LeftVol * SampleTraits<SampleType>::normalize(sample[baseIdx]);
         }

         SrcPos += MixStep;
         TotalSamples--;
      }
   }

   *MixDest = dest;
   return SrcPos;
}

//********************************************************************************************************************
// Vectorized mixer for high-performance scenarios (when sample count is large)

#ifdef __AVX2__
template<typename SampleType>
static int mix_vectorized_mono_to_stereo(APTR Src, int SrcPos, int TotalSamples,
                                         float LeftVol, float RightVol, float **MixDest)
{
   // Use vectorized version only for sufficiently large sample counts
   if (TotalSamples < 8) {
      return mix_template<SampleType, false, true, false>(Src, SrcPos, TotalSamples, 1, LeftVol, RightVol, MixDest);
   }

   auto dest = *MixDest;
   auto sample = (SampleType *)Src;

   const __m256 leftVol = _mm256_set1_ps(LeftVol);
   const __m256 rightVol = _mm256_set1_ps(RightVol);

   // Process 4 samples at a time (AVX can handle 8 floats, we need 2 per sample for stereo)
   while (TotalSamples >= 4) {
      // Load 4 samples
      __m128i samples;
      if constexpr (std::is_same_v<SampleType, uint8_t>) {
         samples = _mm_set_epi32(sample[(SrcPos + 3*MixStep) >> 16],
                                sample[(SrcPos + 2*MixStep) >> 16],
                                sample[(SrcPos + MixStep) >> 16],
                                sample[SrcPos >> 16]);
      } else {
         samples = _mm_set_epi32(sample[(SrcPos + 3*MixStep) >> 16],
                                sample[(SrcPos + 2*MixStep) >> 16],
                                sample[(SrcPos + MixStep) >> 16],
                                sample[SrcPos >> 16]);
      }

      // Convert to float and apply normalization
      __m256 normalized;
      if constexpr (std::is_same_v<SampleType, uint8_t>) {
         __m256 temp = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(samples));
         normalized = _mm256_mul_ps(_mm256_sub_ps(temp, _mm256_set1_ps(128.0f)), _mm256_set1_ps(256.0f));
      } else {
         normalized = _mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(samples));
      }

      // Create left and right channels
      __m256 left = _mm256_mul_ps(normalized, leftVol);
      __m256 right = _mm256_mul_ps(normalized, rightVol);

      // Interleave and store
      __m256 destVec = _mm256_loadu_ps(dest);
      __m256 interleaved_lo = _mm256_unpacklo_ps(left, right);
      __m256 interleaved_hi = _mm256_unpackhi_ps(left, right);

      _mm256_storeu_ps(dest, _mm256_add_ps(destVec, interleaved_lo));
      _mm256_storeu_ps(dest + 4, _mm256_add_ps(_mm256_loadu_ps(dest + 4), interleaved_hi));

      dest += 8;
      SrcPos += 4 * MixStep;
      TotalSamples -= 4;
   }

   // Handle remaining samples with scalar code
   *MixDest = dest;
   if (TotalSamples > 0) {
      return mix_template<SampleType, false, true, false>(Src, SrcPos, TotalSamples, 1, LeftVol, RightVol, MixDest);
   }

   return SrcPos;
}
#endif
