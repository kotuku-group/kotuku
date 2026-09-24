#include <kotuku/main.h>
#include <kotuku/modules/audio.h>
#include <kotuku/modules/script.h>
#include <kotuku/modules/filesystem.h>
#include <algorithm>
#include <cassert>
#include <thread>
#include <future>
#include "audio.h"
#include "audio_effect.cpp"

// Processors deliberately do not commute, so these assertions detect reversed chain order.
class TestProcessor : public AudioEffectProcessor {
public:
   int Resets = 0;
   int Calls = 0;
   float Gain;
   float Offset;

   TestProcessor(float GainValue, float OffsetValue) : Gain(GainValue), Offset(OffsetValue) { }
   void process(float *Buffer, int Frames) override {
      ++Calls;
      for (int i = 0; i < Frames; ++i) Buffer[i] = Buffer[i] * Gain + Offset;
   }
   void reset() override { ++Resets; }
};

static void attach(AudioEffectChain &Chain, extAudioEffect &Effect)
{
   Effect.Sequence = Chain.NextSequence++;
   Chain.Effects.push_back(&Effect);
   sort_effects(Chain);
}

int main()
{
   auto mutex = std::make_shared<std::recursive_mutex>();
   auto chain = std::make_shared<AudioEffectChain>(mutex);
   extAudioEffect first(nullptr, 1), second(nullptr, 2);
   first.Chain = second.Chain = chain;
   first.Order = 2;
   second.Order = 1;
   attach(*chain, first);
   attach(*chain, second);
   auto gain = std::make_unique<TestProcessor>(2, 0);
   auto bias = std::make_unique<TestProcessor>(1, 3);
   auto gain_ptr = gain.get();
   auto bias_ptr = bias.get();
   assert(first.set_processor(std::move(gain)) IS ERR::Okay);
   assert(second.set_processor(std::move(bias)) IS ERR::Okay);
   float samples[] = { 1, -1 };
   process_effects(*chain, samples, 2);
   assert(samples[0] IS 8 and samples[1] IS 4);
   assert(gain_ptr->Resets IS 1 and bias_ptr->Resets IS 1);

   second.Order = 2;
   sort_effects(*chain);
   samples[0] = 1;
   process_effects(*chain, samples, 1);
   assert(samples[0] IS 5); // Equal orders revert to attachment order, not previous sorted order.
   assert(gain_ptr->Resets IS 1);

   first.Flags = AEF::BYPASS;
   samples[0] = 1;
   process_effects(*chain, samples, 1);
   assert(samples[0] IS 4 and gain_ptr->Calls IS 2);
   configure_effects(*chain, 48000, true);
   assert(first.OutputRate IS 48000 and first.Stereo IS 1);
   first.Flags = AEF::NIL;
   samples[0] = 1;
   process_effects(*chain, samples, 1);
   assert(gain_ptr->Resets IS 2 and bias_ptr->Resets IS 2);

   // Detach must wait for an in-flight window before the caller can destroy DSP state.
   std::promise<void> started;
   std::future<void> detached;
   {
      std::lock_guard lock(*mutex);
      detached = std::async(std::launch::async, [&] {
         started.set_value();
         first.detach();
      });
      started.get_future().wait();
      assert(detached.wait_for(std::chrono::milliseconds(20)) IS std::future_status::timeout);
      assert(chain->Effects.size() IS 2);
   }
   detached.get();
   assert(chain->Effects.size() IS 1 and chain->Effects[0] IS &second);
   chain.reset(); // Target destruction before effect destruction must be safe.
   second.detach();
   return 0;
}
