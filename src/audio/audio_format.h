// Validation and derivation rules for the public AudioFormat descriptor and channel layouts.  The public descriptor can
// represent layouts that the mixer cannot yet play; playback support is checked separately from validity.

#pragma once

#include <algorithm>
#include <bit>
#include <span>
#include <string>
#include <vector>

constexpr int MAX_FORMAT_CHANNELS = 64;     // Implementation limit for any AudioFormat layout
constexpr int MAX_FORMAT_RATE     = 768000; // Highest nominal sample rate accepted by a descriptor
constexpr int DISCRETE_CHANNELS   = 0x10000;

//********************************************************************************************************************
// Private mixer source formats.  These are the representations that the mono/stereo mixer can dispatch.

enum class PCM : int { NIL = 0, U8_MONO, S16_MONO, U8_STEREO, S16_STEREO };

inline bool pcm_stereo(PCM Format)
{
   return (Format IS PCM::U8_STEREO) or (Format IS PCM::S16_STEREO);
}

inline bool pcm_16bit(PCM Format)
{
   return (Format IS PCM::S16_MONO) or (Format IS PCM::S16_STEREO);
}

//********************************************************************************************************************
// An owned copy of an AudioFormat.  Descriptors are copied on registration so that callers retain their storage.

struct PcmFormat {
   std::vector<int> Layout;
   int SampleRate   = 0;
   ASF SampleFormat = ASF::NIL;
   AFF Flags        = AFF::NIL;

   int channels() const { return int(Layout.size()); }
};

//********************************************************************************************************************

inline int sample_bytes(ASF Format)
{
   switch (Format) {
      case ASF::U8:  return 1;
      case ASF::S16: return 2;
      case ASF::F32: return 4;
      default:       return 0;
   }
}

inline bool valid_channel_identity(int Identity)
{
   return ((Identity >= int(SPK::CENTRE)) and (Identity <= int(SPK::REAR_RIGHT))) or
      ((Identity >= int(SPK::DISCRETE)) and (Identity < int(SPK::DISCRETE) + DISCRETE_CHANNELS));
}

//********************************************************************************************************************
// A layout is malformed if it is empty, exceeds the channel limit, or contains an invalid or duplicated identity.

inline ERR validate_layout(std::span<const int> Layout)
{
   if (Layout.empty() or (Layout.size() > size_t(MAX_FORMAT_CHANNELS))) return ERR::Args;
   for (size_t i = 0; i < Layout.size(); i++) {
      if (not valid_channel_identity(Layout[i])) return ERR::Args;
      for (size_t j = 0; j < i; j++) {
         if (Layout[j] IS Layout[i]) return ERR::Args;
      }
   }
   return ERR::Okay;
}

inline std::span<const int> format_layout(const AudioFormat &Format)
{
   return std::span<const int>(Format.Layout.data(), Format.Layout.size());
}

inline ERR validate_format(const AudioFormat &Format)
{
   if ((Format.SampleRate < 1) or (Format.SampleRate > MAX_FORMAT_RATE)) return ERR::Args;
   if (not sample_bytes(Format.SampleFormat)) return ERR::Args;
   if ((Format.Flags & ~AFF::BIG_ENDIAN_ORDER) != AFF::NIL) return ERR::Args;
   return validate_layout(format_layout(Format));
}

//********************************************************************************************************************
// Size of one interleaved frame.  The limits enforced by validate_format() keep the result within an int, but the
// arithmetic is checked in case those limits are raised.

inline ERR format_frame_bytes(const AudioFormat &Format, int &Bytes)
{
   Bytes = 0;
   if (auto error = validate_format(Format); error != ERR::Okay) return error;
   const int64_t bytes = int64_t(sample_bytes(Format.SampleFormat)) * int64_t(Format.Layout.size());
   if (bytes > INT_MAX) return ERR::OutOfRange;
   Bytes = int(bytes);
   return ERR::Okay;
}

inline int format_frame_bytes(const PcmFormat &Format)
{
   return sample_bytes(Format.SampleFormat) * Format.channels();
}

inline PcmFormat copy_format(const AudioFormat &Format)
{
   PcmFormat result;
   result.Layout.assign(Format.Layout.begin(), Format.Layout.end());
   result.SampleRate   = Format.SampleRate;
   result.SampleFormat = Format.SampleFormat;
   result.Flags        = Format.Flags;
   return result;
}

// True if multi-byte samples in this format must be byte-swapped to native order.

inline bool format_needs_swap(ASF SampleFormat, AFF Flags)
{
   if (sample_bytes(SampleFormat) < 2) return false;
   const bool big = (Flags & AFF::BIG_ENDIAN_ORDER) != AFF::NIL;
   return big != (std::endian::native IS std::endian::big);
}

inline void swap_samples_16(uint8_t *Data, size_t Bytes)
{
   for (size_t i = 0; i + 1 < Bytes; i += 2) std::swap(Data[i], Data[i + 1]);
}

//********************************************************************************************************************
// Layout presets.  Each preset expands to an ordered identity list; a channel count alone never selects a layout.

static const int glLayoutMono[]   = { int(SPK::CENTRE) };
static const int glLayoutStereo[] = { int(SPK::FRONT_LEFT), int(SPK::FRONT_RIGHT) };
static const int glLayoutSide[]   = { int(SPK::FRONT_LEFT), int(SPK::FRONT_RIGHT), int(SPK::CENTRE), int(SPK::LFE),
   int(SPK::SIDE_LEFT), int(SPK::SIDE_RIGHT) };
static const int glLayoutRear[]   = { int(SPK::FRONT_LEFT), int(SPK::FRONT_RIGHT), int(SPK::CENTRE), int(SPK::LFE),
   int(SPK::REAR_LEFT), int(SPK::REAR_RIGHT) };

inline std::span<const int> preset_layout(ACL Preset)
{
   switch (Preset) {
      case ACL::MONO:              return glLayoutMono;
      case ACL::STEREO:            return glLayoutStereo;
      case ACL::SURROUND_5_1_SIDE: return glLayoutSide;
      case ACL::SURROUND_5_1_REAR: return glLayoutRear;
      default:                     return {};
   }
}

inline bool layout_is(std::span<const int> Layout, std::span<const int> Preset)
{
   return std::equal(Layout.begin(), Layout.end(), Preset.begin(), Preset.end());
}

// The mixer currently renders mono (CENTRE) and stereo (FRONT_LEFT, FRONT_RIGHT) only.

inline bool output_layout_supported(std::span<const int> Layout)
{
   return layout_is(Layout, glLayoutMono) or layout_is(Layout, glLayoutStereo);
}

// Returns the mixer format for a source, or PCM::NIL if the representation or layout cannot be played.

inline PCM playback_format(ASF SampleFormat, std::span<const int> Layout)
{
   const bool mono = layout_is(Layout, glLayoutMono);
   const bool stereo = layout_is(Layout, glLayoutStereo);
   if (SampleFormat IS ASF::U8) return mono ? PCM::U8_MONO : (stereo ? PCM::U8_STEREO : PCM::NIL);
   if (SampleFormat IS ASF::S16) return mono ? PCM::S16_MONO : (stereo ? PCM::S16_STEREO : PCM::NIL);
   return PCM::NIL;
}

//********************************************************************************************************************
// Stable key and label fragments for channel identities, used to build meter descriptor keys such as
// `input_peak_left`.  Unknown identities return an empty string.

inline std::string channel_key(int Identity)
{
   switch (Identity) {
      case int(SPK::CENTRE):      return "centre";
      case int(SPK::FRONT_LEFT):  return "left";
      case int(SPK::FRONT_RIGHT): return "right";
      case int(SPK::LFE):         return "lfe";
      case int(SPK::SIDE_LEFT):   return "side_left";
      case int(SPK::SIDE_RIGHT):  return "side_right";
      case int(SPK::REAR_LEFT):   return "rear_left";
      case int(SPK::REAR_RIGHT):  return "rear_right";
      default:
         if (valid_channel_identity(Identity)) return "discrete_" + std::to_string(Identity - int(SPK::DISCRETE));
         return std::string();
   }
}

inline std::string channel_label(int Identity)
{
   switch (Identity) {
      case int(SPK::CENTRE):      return "Centre";
      case int(SPK::FRONT_LEFT):  return "Left";
      case int(SPK::FRONT_RIGHT): return "Right";
      case int(SPK::LFE):         return "LFE";
      case int(SPK::SIDE_LEFT):   return "Side Left";
      case int(SPK::SIDE_RIGHT):  return "Side Right";
      case int(SPK::REAR_LEFT):   return "Rear Left";
      case int(SPK::REAR_RIGHT):  return "Rear Right";
      default:
         if (valid_channel_identity(Identity)) return "Discrete " + std::to_string(Identity - int(SPK::DISCRETE));
         return std::string();
   }
}
