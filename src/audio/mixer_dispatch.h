#pragma once

struct MixingParams {
   APTR src;
   int src_pos;
   int total_samples;
   int next_sample_offset;
   float left_vol;
   float right_vol;
   float **mix_dest;
};

// Audio configuration structure
struct AudioConfig {
   bool stereo_output;
   bool use_interpolation;

   // Default constructor
   AudioConfig() : stereo_output(false), use_interpolation(false) {}

   AudioConfig(bool stereo_out, bool interpolation)
      : stereo_output(stereo_out), use_interpolation(interpolation) {}
};

// Primary mixing dispatch function
class AudioMixer {
public:
   static int dispatch_mix(const AudioConfig& config, SFM sample_format, const MixingParams& params);
};
