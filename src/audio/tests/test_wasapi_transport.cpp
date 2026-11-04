// Included by audio.cpp to exercise the module implementation.

namespace audio_tests_wasapi_transport {

struct Source {
   AudioTestContext &Test;
   std::atomic<unsigned> Calls{0}, ShortPackets{0};
   std::atomic<bool> Active{true}, Failed{false};
};

static bool produce(void *Context, float *Buffer, unsigned Frames, unsigned Padding)
{
   auto source = (Source *)Context;
   if (!source->Test.check(Frames <= 768 and Padding <= 768, "Packet exceeds capacity", __FILE__, __LINE__))
      return false;
   if (Frames IS 127) ++source->ShortPackets;
   for (unsigned i = 0; i < Frames * 2; ++i) Buffer[i] = source->Active ? 0.25f : 0.0f;
   ++source->Calls;
   return source->Active;
}

static void failed(void *Context, int Error) {
   ((Source *)Context)->Test.check(Error != 0, "Failure callback requires an error", __FILE__, __LINE__);
   ((Source *)Context)->Failed = true;
}

static void run(AudioTestContext &Test)
{
   AudioEndpointFormat surround{6, 32, 32, 0x3f, true};
   AUDIO_CHECK(surround.valid());
   float input[] = {0.25f, -0.5f, 2.0f, -2.0f};
   float output[12];
   surround.write(output, input, 2);
   AUDIO_CHECK(output[0] IS 0.25f and output[1] IS -0.5f and output[6] IS 1.0f and output[7] IS -1.0f);
   for (unsigned i : {2u, 3u, 4u, 5u, 8u, 9u, 10u, 11u}) AUDIO_CHECK(output[i] IS 0);
   surround.Mask = 0;
   AUDIO_CHECK(!surround.valid());
   AudioEndpointFormat pcm{2, 24, 24, 3, false};
   uint8_t packed[12];
   pcm.write(packed, input, 2);
   AUDIO_CHECK(packed[2] IS 0x20 and packed[5] IS 0xc0 and packed[8] IS 0x7f and packed[11] IS 0x80);
   pcm.Bits = 32;
   uint8_t padded[16];
   pcm.write(padded, input, 2);
   for (unsigned i : {0u, 4u, 8u, 12u}) AUDIO_CHECK(padded[i] IS 0);
   for (unsigned rate : {44100u, 48000u, 96000u}) {
      Source source{Test};
      WasapiFormat format{rate, 2};
      auto stream = wasapi_open("null", format, &source, produce, failed);
      AUDIO_CHECK(stream and format.Rate IS rate and format.Capacity >= format.Period);
      if (!stream) continue;
      wasapi_start(stream);
      for (unsigned i = 0; i < 20; ++i) {
         wasapi_wake(stream);
         std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      source.Active = false;
      wasapi_wake(stream);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      source.Active = true;
      wasapi_wake(stream);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      AUDIO_CHECK(source.Calls > 3 and source.ShortPackets > 0);
      wasapi_test_invalidate(stream);
      for (unsigned i = 0; i < 100 and !source.Failed; ++i)
         std::this_thread::sleep_for(std::chrono::milliseconds(1));
      AUDIO_CHECK(source.Failed);
      wasapi_close(stream);
   }
   // Stop a waiting, never-started stream and reject a bad endpoint without leaking a worker.
   Source source{Test};
   WasapiFormat format{48000, 2};
   auto idle = wasapi_open("null", format, &source, produce, failed);
   AUDIO_CHECK(idle);
   wasapi_close(idle);
   auto changed = wasapi_open("null", format, &source, produce, failed);
   AUDIO_CHECK(changed);
   if (!changed) return;
   wasapi_test_invalidate(changed);
   std::this_thread::sleep_for(std::chrono::milliseconds(10));
   AUDIO_CHECK(!source.Failed); // Failure delivery must wait until the client has published and started the stream.
   wasapi_start(changed);
   for (unsigned i = 0; i < 100 and !source.Failed; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
   AUDIO_CHECK(source.Failed);
   wasapi_close(changed);
   auto invalid = wasapi_open("Kotuku-nonexistent-endpoint", format, &source, produce, failed);
   AUDIO_CHECK(!invalid);
   wasapi_close(invalid);
}

} // namespace audio_tests_wasapi_transport
