// Format descriptor rules, byte-order conversion and WAVE layout mapping.  Included by audio.cpp.

namespace audio_tests_audio_format {

static AudioFormat make_format(ASF SampleFormat, std::span<const int> Layout, AFF Flags = AFF::NIL)
{
   AudioFormat format{};
   format.SampleRate   = 48000;
   format.SampleFormat = SampleFormat;
   format.Flags        = Flags;
   format.Layout.assign(Layout.begin(), Layout.end());
   return format;
}

static void validation(AudioTestContext &Test)
{
   int bytes;
   AUDIO_CHECK(format_frame_bytes(make_format(ASF::S16, glLayoutSide), bytes) IS ERR::Okay and bytes IS 12);
   AUDIO_CHECK(format_frame_bytes(make_format(ASF::F32, glLayoutStereo), bytes) IS ERR::Okay and bytes IS 8);
   AUDIO_CHECK(format_frame_bytes(make_format(ASF::NIL, glLayoutMono), bytes) IS ERR::Args and bytes IS 0);

   const int duplicate[] = { int(SPK::LFE), int(SPK::LFE) };
   const int discrete[] = { int(SPK::DISCRETE), int(SPK::DISCRETE) + 0xffff };
   const int beyond[] = { int(SPK::DISCRETE) + 0x10000 };
   AUDIO_CHECK(validate_layout(duplicate) IS ERR::Args);
   AUDIO_CHECK(validate_layout(discrete) IS ERR::Okay);
   AUDIO_CHECK(validate_layout(beyond) IS ERR::Args);
   AUDIO_CHECK(validate_layout({}) IS ERR::Args);

   // Valid but unsupported formats have no mixer representation.

   AUDIO_CHECK(playback_format(ASF::S16, glLayoutSide) IS PCM::NIL);
   AUDIO_CHECK(playback_format(ASF::F32, glLayoutStereo) IS PCM::NIL);
   AUDIO_CHECK(playback_format(ASF::S16, discrete) IS PCM::NIL);
   AUDIO_CHECK(playback_format(ASF::U8, glLayoutMono) IS PCM::U8_MONO);
   AUDIO_CHECK(playback_format(ASF::S16, glLayoutStereo) IS PCM::S16_STEREO);
   AUDIO_CHECK(output_layout_supported(glLayoutMono) and !output_layout_supported(glLayoutRear));

   // The registered copy is independent of the caller's descriptor.

   auto source = make_format(ASF::S16, glLayoutStereo);
   auto copy = copy_format(source);
   source.Layout.clear();
   AUDIO_CHECK(copy.channels() IS 2 and format_frame_bytes(copy) IS 4);
}

// Big endian and little endian registrations of the same PCM must produce identical native samples.

static void byte_order(AudioTestContext &Test)
{
   const int16_t native[] = { 1, -2, 0x1234, -32768 };
   std::array<uint8_t, sizeof(native)> little, big;
   for (size_t i = 0; i < std::size(native); i++) {
      const auto value = uint16_t(native[i]);
      little[i * 2] = big[i * 2 + 1] = uint8_t(value);
      little[i * 2 + 1] = big[i * 2] = uint8_t(value >> 8);
   }

   const bool little_swap = format_needs_swap(ASF::S16, AFF::NIL);
   const bool big_swap = format_needs_swap(ASF::S16, AFF::BIG_ENDIAN_ORDER);
   AUDIO_CHECK(little_swap != big_swap);
   AUDIO_CHECK(!format_needs_swap(ASF::U8, AFF::BIG_ENDIAN_ORDER));

   if (little_swap) swap_samples_16(little.data(), little.size());
   if (big_swap) swap_samples_16(big.data(), big.size());
   AUDIO_CHECK(little IS big);
   AUDIO_CHECK(std::memcmp(little.data(), native, sizeof(native)) IS 0);
}

static void wave_layouts(AudioTestContext &Test)
{
   kt::vector<int> layout;
   wave_layout(1, 0, layout);
   AUDIO_CHECK(layout.size() IS 1 and layout[0] IS int(SPK::CENTRE));
   wave_layout(2, 0, layout);
   AUDIO_CHECK(layout.size() IS 2 and layout[1] IS int(SPK::FRONT_RIGHT));

   // Without a speaker mask, a channel count alone does not identify a surround arrangement.

   wave_layout(6, 0, layout);
   AUDIO_CHECK(layout.size() IS 6 and layout[0] IS int(SPK::DISCRETE) and layout[5] IS int(SPK::DISCRETE) + 5);

   // KSAUDIO_SPEAKER_5POINT1 (rear) and KSAUDIO_SPEAKER_5POINT1_SURROUND (side).

   wave_layout(6, 0x3f, layout);
   AUDIO_CHECK(std::equal(layout.begin(), layout.end(), std::begin(glLayoutRear), std::end(glLayoutRear)));
   wave_layout(6, 0x60f, layout);
   AUDIO_CHECK(std::equal(layout.begin(), layout.end(), std::begin(glLayoutSide), std::end(glLayoutSide)));

   // Speakers without an identity, and channels beyond the mask, become discrete channels.

   wave_layout(3, 0x40 | 0x1, layout);
   AUDIO_CHECK(layout.size() IS 3 and layout[0] IS int(SPK::FRONT_LEFT) and layout[1] IS int(SPK::DISCRETE) and
      layout[2] IS int(SPK::DISCRETE) + 1);
   AUDIO_CHECK(validate_layout(std::span<const int>(layout.data(), layout.size())) IS ERR::Okay);
}

static void wave_export_formats(AudioTestContext &Test)
{
   AUDIO_CHECK(wave_export_format_supported(make_format(ASF::U8, glLayoutMono, AFF::BIG_ENDIAN_ORDER)));
   AUDIO_CHECK(!wave_export_format_supported(make_format(ASF::S16, glLayoutMono, AFF::BIG_ENDIAN_ORDER)));
   AUDIO_CHECK(!wave_export_format_supported(make_format(ASF::F32, glLayoutStereo, AFF::BIG_ENDIAN_ORDER)));
   AUDIO_CHECK(wave_export_format_supported(make_format(ASF::S16, glLayoutStereo)));
   AUDIO_CHECK(!wave_export_format_supported(make_format(ASF::S16, glLayoutSide)));
}

static void run(AudioTestContext &Test)
{
   validation(Test);
   byte_order(Test);
   wave_layouts(Test);
   wave_export_formats(Test);
}

} // namespace audio_tests_audio_format
