#include "mixer_dispatch.h"

//********************************************************************************************************************
// Main dispatch function that replaces the MixRoutines arrays

int AudioMixer::dispatch_mix(const AudioConfig& config, SFM sample_format, const MixingParams& params)
{
   // Try SIMD optimization first for supported mono-to-stereo cases
   if (config.stereo_output and !config.use_interpolation and params.total_samples >= 8) {
      if (sample_format IS SFM::U8_BIT_MONO) {
#ifdef __AVX2__
         return mix_vectorized_mono_to_stereo<uint8_t>(
            params.src, params.src_pos, params.total_samples,
            params.left_vol, params.right_vol, params.mix_dest);
#endif
      }
      else if (sample_format IS SFM::S16_BIT_MONO) {
#ifdef __AVX2__
         return mix_vectorized_mono_to_stereo<int16_t>(
            params.src, params.src_pos, params.total_samples,
            params.left_vol, params.right_vol, params.mix_dest);
#endif
      }
   }

   // Direct dispatch to the appropriate mix_template instantiation
   switch (sample_format) {
      case SFM::U8_BIT_MONO:
         if (config.stereo_output) {
            if (config.use_interpolation) {
               return mix_template<uint8_t, false, true, true>(params.src, params.src_pos, params.total_samples, params.next_sample_offset, params.left_vol, params.right_vol, params.mix_dest);
            }
            else return mix_template<uint8_t, false, true, false>(params.src, params.src_pos, params.total_samples, params.next_sample_offset, params.left_vol, params.right_vol, params.mix_dest);
         }
         else {
            if (config.use_interpolation) {
               return mix_template<uint8_t, false, false, true>(params.src, params.src_pos, params.total_samples, params.next_sample_offset, params.left_vol, params.right_vol, params.mix_dest);
            }
            else return mix_template<uint8_t, false, false, false>(params.src, params.src_pos, params.total_samples, params.next_sample_offset, params.left_vol, params.right_vol, params.mix_dest);
         }

      case SFM::U8_BIT_STEREO:
         if (config.stereo_output) {
            if (config.use_interpolation) {
               return mix_template<uint8_t, true, true, true>(params.src, params.src_pos, params.total_samples, params.next_sample_offset, params.left_vol, params.right_vol, params.mix_dest);
            }
            else return mix_template<uint8_t, true, true, false>(params.src, params.src_pos, params.total_samples, params.next_sample_offset, params.left_vol, params.right_vol, params.mix_dest);
         }
         else {
            if (config.use_interpolation) {
               return mix_template<uint8_t, true, false, true>(params.src, params.src_pos, params.total_samples, params.next_sample_offset, params.left_vol, params.right_vol, params.mix_dest);
            }
            else return mix_template<uint8_t, true, false, false>(params.src, params.src_pos, params.total_samples, params.next_sample_offset, params.left_vol, params.right_vol, params.mix_dest);
         }

      case SFM::S16_BIT_MONO:
         if (config.stereo_output) {
            if (config.use_interpolation) {
               return mix_template<int16_t, false, true, true>(params.src, params.src_pos, params.total_samples, params.next_sample_offset, params.left_vol, params.right_vol, params.mix_dest);
            }
            else return mix_template<int16_t, false, true, false>(params.src, params.src_pos, params.total_samples, params.next_sample_offset, params.left_vol, params.right_vol, params.mix_dest);
         }
         else {
            if (config.use_interpolation) {
               return mix_template<int16_t, false, false, true>(params.src, params.src_pos, params.total_samples, params.next_sample_offset, params.left_vol, params.right_vol, params.mix_dest);
            }
            else return mix_template<int16_t, false, false, false>(params.src, params.src_pos, params.total_samples, params.next_sample_offset, params.left_vol, params.right_vol, params.mix_dest);
         }

      case SFM::S16_BIT_STEREO:
         if (config.stereo_output) {
            if (config.use_interpolation) {
               return mix_template<int16_t, true, true, true>(params.src, params.src_pos, params.total_samples, params.next_sample_offset, params.left_vol, params.right_vol, params.mix_dest);
            }
            else return mix_template<int16_t, true, true, false>(params.src, params.src_pos, params.total_samples, params.next_sample_offset, params.left_vol, params.right_vol, params.mix_dest);
         }
         else {
            if (config.use_interpolation) {
               return mix_template<int16_t, true, false, true>(params.src, params.src_pos, params.total_samples, params.next_sample_offset, params.left_vol, params.right_vol, params.mix_dest);
            }
            else return mix_template<int16_t, true, false, false>(params.src, params.src_pos, params.total_samples, params.next_sample_offset, params.left_vol, params.right_vol, params.mix_dest);
         }

      default:
         return 0; // Invalid format
   }
}
