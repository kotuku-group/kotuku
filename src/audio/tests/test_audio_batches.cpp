#include <kotuku/main.h>
#include <kotuku/modules/audio.h>
#include <kotuku/modules/script.h>
#include <kotuku/modules/filesystem.h>
#include <algorithm>
#include <cassert>
#include <barrier>
#include <thread>
#include "audio.h"
#include "audio_batch.h"

int main()
{
   ChannelSet set;
   set.Channel.resize(2);
   set.Commands.reserve(1024);
   constexpr int handle = 1 << 16;
   std::array<AudioMixCommand, 2> batch = {{
      { MIX::VOLUME, handle, 0, 0.25 }, { MIX::PAN, handle + 1, 0, -0.5 }
   }};
   assert(submit_audio_batch(set, 1, batch) IS ERR::Okay);
   batch[0].Value = 1;
   assert(std::get<double>(set.Commands[0].Data) IS 0.25); // Admission owns a copy.
   assert(set.Commands[1].Handle IS handle + 1);
   assert(set.Commands[2].CommandID IS CMD::END_SEQUENCE);

   // Invalid suffixes must not publish an otherwise valid prefix.
   batch[1].Handle = 2 << 16;
   assert(submit_audio_batch(set, 1, batch) IS ERR::OutOfRange);
   batch[1].Handle = handle + 2;
   assert(submit_audio_batch(set, 1, batch) IS ERR::OutOfRange);
   batch[1].Handle = handle;
   batch[1].Operation = MIX(999);
   assert(submit_audio_batch(set, 1, batch) IS ERR::Args);
   batch[1].Operation = MIX::PAN;
   batch[1].Value = std::numeric_limits<double>::quiet_NaN();
   assert(submit_audio_batch(set, 1, batch) IS ERR::OutOfRange);
   assert(set.Commands.size() IS 3);
   batch[1].Value = 0;

   std::vector<AudioMixCommand> full(1023, batch[0]);
   assert(submit_audio_batch(set, 1, full) IS ERR::BufferOverflow);
   assert(set.Commands.size() IS 3);
   set.Commands.clear();
   assert(submit_audio_batch(set, 1, full) IS ERR::Okay);
   assert(set.Commands.size() IS 1024);
   assert(submit_audio_batch(set, 1, batch) IS ERR::BufferOverflow);
   assert(set.Commands.back().CommandID IS CMD::END_SEQUENCE);
   full.push_back(batch[0]);
   set.Commands.clear();
   assert(submit_audio_batch(set, 1, full) IS ERR::BufferOverflow);
   assert(submit_audio_batch(set, 1, {}) IS ERR::NullArgs);
   assert(set.Commands.empty());

   // One boundary consumes only the first batch, preserving cross-channel array order and later batches.
   batch[1].Handle = handle + 1;
   assert(submit_audio_batch(set, 1, batch) IS ERR::Okay);
   assert(submit_audio_batch(set, 1, batch) IS ERR::Okay);
   std::vector<int> applied;
   auto apply = [&](const AudioCommand &Command) { applied.push_back(Command.Handle); };
   execute_next_audio_batch(set, apply);
   assert(applied.size() IS 2 and applied[0] IS handle and applied[1] IS handle + 1);
   assert(set.Commands.size() IS 3);
   execute_next_audio_batch(set, apply);
   assert(applied.size() IS 4 and set.Commands.empty());
   execute_next_audio_batch(set, apply);
   assert(applied.size() IS 4);

   // Both producers assemble their arrays before either submits.  Submission order can vary; boundaries cannot.
   std::mutex mutex;
   std::barrier ready(2);
   auto producer = [&](int Handle) {
      std::array<AudioMixCommand, 2> commands = {{
         { MIX::FREQUENCY, Handle, 22050, 0 }, { MIX::PLAY, Handle, 0, 0 }
      }};
      ready.arrive_and_wait();
      for (int i = 0; i < 100; ++i) {
         std::lock_guard lock(mutex);
         assert(submit_audio_batch(set, 1, commands) IS ERR::Okay);
      }
   };
   std::thread first(producer, handle), second(producer, handle + 1);
   first.join();
   second.join();
   assert(set.Commands.size() IS 600);
   for (size_t i = 0; i < set.Commands.size(); i += 3) {
      assert(set.Commands[i].CommandID IS CMD::FREQUENCY);
      assert(set.Commands[i + 1].CommandID IS CMD::PLAY);
      assert(set.Commands[i].Handle IS set.Commands[i + 1].Handle);
      assert(set.Commands[i + 2].CommandID IS CMD::END_SEQUENCE);
   }
   set.clear();
   assert(set.Commands.empty());
}
