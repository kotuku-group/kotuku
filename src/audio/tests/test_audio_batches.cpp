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
   // Completion storage is bounded and releases capacity only when delivery takes ownership.
   AudioCompletions completions;
   FUNCTION callback;
   int first_slot = completions.reserve(3, callback);
   int second_slot = completions.reserve(1, callback);
   AudioCompletions::Entry result;
   assert(!completions.take(result));
   completions.complete(first_slot, 0, ERR::Okay);
   completions.complete(first_slot, 1, ERR::NoData);
   assert(!completions.take(result));
   completions.complete(second_slot, 0, ERR::Okay);
   completions.complete(first_slot, 2, ERR::Cancelled);
   completions.complete(first_slot, 2, ERR::Cancelled); // Do not publish twice.
   assert(completions.available() IS 2);
   assert(completions.take(result) and result.Error IS ERR::Okay and result.FailedCommand IS -1);
   assert(completions.take(result) and result.Error IS ERR::NoData and result.FailedCommand IS 1);
   assert(completions.pending() IS 0 and !completions.take(result));
   for (int i = 0; i < 1024; ++i) {
      int slot = completions.reserve(1, callback);
      assert(slot >= 0);
      completions.complete(slot, 0, ERR::Cancelled);
   }
   assert(completions.reserve(1, callback) IS -1);
   assert(completions.take(result) and result.Error IS ERR::Cancelled and result.FailedCommand IS 0);
   int slot = completions.reserve(1, callback);
   assert(slot >= 0);
   completions.complete(slot, 0, ERR::Okay);
   for (int i = 0; i < 1023; ++i) {
      assert(completions.take(result) and result.Error IS ERR::Cancelled);
   }
   assert(completions.take(result) and result.Error IS ERR::Okay);
   assert(completions.pending() IS 0);
   assert(completions.reserve(1, callback) >= 0);
   int released = 0;
   completions.clear([&](FUNCTION &) { ++released; });
   assert(released IS 1 and completions.pending() IS 0 and completions.available() IS 0);

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

   // Playback positions retain their full 64-bit value through batch staging.
   constexpr int64_t large_position = int64_t(INT32_MAX) + 4097;
   std::array<AudioMixCommand, 1> play = {{{ MIX::PLAY, handle, large_position, 0 }}};
   assert(submit_audio_batch(set, 1, play) IS ERR::Okay);
   assert(std::get<int64_t>(set.Commands.front().Data) IS large_position);
   set.Commands.clear();

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

   // Documented frame counts, including odd-frame ties and the minimum non-zero tick.
   set.Tempo = 125;
   assert(set.TickFrames(22050) IS SAMPLE(442));
   assert(set.TickFrames(44100) IS SAMPLE(882));
   assert(set.TickFrames(48000) IS SAMPLE(960));
   set.Tempo = 128;
   assert(set.TickFrames(44100) IS SAMPLE(862));
   set.Tempo = 1;
   assert(set.TickFrames(192000) IS SAMPLE(480000));
   set.Tempo = 100000;
   assert(set.TickFrames(8000) IS SAMPLE(2));
   assert(set.TickFrames(192000) IS SAMPLE(4));

   // A single command during playback preserves the current tick's remaining frames.
   set.Tempo = 125;
   set.MixLeft = set.TickFrames(44100);
   auto apply_tempo = [&](const AudioCommand &Command) {
      if (Command.CommandID IS CMD::TEMPO) set.Tempo = std::get<int>(Command.Data);
      else applied.push_back(Command.Handle);
   };
   advance_audio_tick(set, SAMPLE(400), 44100, apply_tempo);
   assert(set.MixLeft IS SAMPLE(482));
   set.Tempo = 250;
   advance_audio_tick(set, SAMPLE(481), 44100, apply_tempo);
   assert(set.MixLeft IS SAMPLE(1));
   advance_audio_tick(set, SAMPLE(1), 44100, apply_tempo);
   assert(set.MixLeft IS SAMPLE(442));

   // A batch tempo change schedules the immediately following tick; the last tempo wins.
   batch = {{{ MIX::TEMPO, handle, 125, 0 }, { MIX::TEMPO, handle + 1, 128, 0 }}};
   assert(submit_audio_batch(set, 1, batch) IS ERR::Okay);
   std::array<AudioMixCommand, 1> following = {{{ MIX::STOP, handle, 0, 0 }}};
   assert(submit_audio_batch(set, 1, following) IS ERR::Okay);
   applied.clear();
   advance_audio_tick(set, SAMPLE(442), 44100, apply_tempo);
   assert(set.Tempo IS 128 and set.MixLeft IS SAMPLE(862));
   assert(applied.empty() and set.Commands.size() IS 2);
   advance_audio_tick(set, SAMPLE(861), 44100, apply_tempo);
   assert(applied.empty());
   advance_audio_tick(set, SAMPLE(1), 44100, apply_tempo);
   assert(applied.size() IS 1 and set.Commands.empty());
   for (int invalid : { 0, -1, 100001 }) {
      batch[1].Integer = invalid;
      assert(submit_audio_batch(set, 1, batch) IS ERR::OutOfRange);
      assert(set.Commands.empty());
   }

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
