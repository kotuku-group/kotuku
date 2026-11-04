#pragma once

// Requested processing profiles. Maximum aliases the measured High sinc kernel.
enum class AudioQualityMode { DRAFT, LOW, STANDARD, HIGH };

struct AudioQualityProfile {
   int Minimum;
   int Maximum;
   AudioQualityMode Mode;
   ResamplerMode Resampler;
};

inline constexpr AudioQualityProfile glQualityProfiles[] = {
   { 0, 9, AudioQualityMode::DRAFT, ResamplerMode::NEAREST },
   { 10, 32, AudioQualityMode::LOW, ResamplerMode::LINEAR },
   { 33, 65, AudioQualityMode::STANDARD, ResamplerMode::CUBIC },
   { 66, 100, AudioQualityMode::HIGH, ResamplerMode::SINC }
};

inline constexpr const AudioQualityProfile * audio_quality_profile(int Quality)
{
   for (const auto &profile : glQualityProfiles) {
      if (Quality >= profile.Minimum and Quality <= profile.Maximum) return &profile;
   }
   return nullptr;
}

inline constexpr const AudioQualityProfile * audio_effective_quality_profile(int Quality)
{
   const auto profile = audio_quality_profile(Quality);
   return profile;
}
