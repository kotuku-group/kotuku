// Included by audio.cpp to exercise the module implementation.

namespace audio_tests_audio_batches {

static void run(AudioTestContext &Test)
{
   // Completion storage is bounded and releases capacity only when delivery takes ownership.
   AudioCompletions completions;
   FUNCTION callback;
   int first_slot = completions.reserve(3, callback);
   int second_slot = completions.reserve(1, callback);
   AudioCompletions::Entry result;
   AUDIO_REQUIRE(!completions.take(result));
   completions.complete(first_slot, 0, ERR::Okay);
   completions.complete(first_slot, 1, ERR::NoData);
   AUDIO_REQUIRE(!completions.take(result));
   completions.complete(second_slot, 0, ERR::Okay);
   completions.complete(first_slot, 2, ERR::Cancelled);
   completions.complete(first_slot, 2, ERR::Cancelled); // Do not publish twice.
   AUDIO_REQUIRE(completions.available() IS 2);
   AUDIO_REQUIRE(completions.take(result) and result.Error IS ERR::Okay and result.FailedCommand IS -1);
   AUDIO_REQUIRE(completions.take(result) and result.Error IS ERR::NoData and result.FailedCommand IS 1);
   AUDIO_REQUIRE(completions.pending() IS 0 and !completions.take(result));
   for (int i = 0; i < 1024; ++i) {
      int slot = completions.reserve(1, callback);
      AUDIO_REQUIRE(slot >= 0);
      completions.complete(slot, 0, ERR::Cancelled);
   }
   AUDIO_REQUIRE(completions.reserve(1, callback) IS -1);
   AUDIO_REQUIRE(completions.take(result) and result.Error IS ERR::Cancelled and result.FailedCommand IS 0);
   int slot = completions.reserve(1, callback);
   AUDIO_REQUIRE(slot >= 0);
   completions.complete(slot, 0, ERR::Okay);
   for (int i = 0; i < 1023; ++i) {
      AUDIO_REQUIRE(completions.take(result) and result.Error IS ERR::Cancelled);
   }
   AUDIO_REQUIRE(completions.take(result) and result.Error IS ERR::Okay);
   AUDIO_REQUIRE(completions.pending() IS 0);
   AUDIO_REQUIRE(completions.reserve(1, callback) >= 0);
   int released = 0;
   completions.clear([&](FUNCTION &) { ++released; });
   AUDIO_REQUIRE(released IS 1 and completions.pending() IS 0 and completions.available() IS 0);

   ChannelSet set;
   set.Channel.resize(2);
   set.Commands.reserve(1024);
   constexpr int handle = 1 << 16;
   std::array<AudioMixCommand, 2> batch = {{
      { MIX::VOLUME, handle, 0, 0.25 }, { MIX::PAN, handle + 1, 0, -0.5 }
   }};
   AUDIO_REQUIRE(submit_audio_batch(set, 1, batch) IS ERR::Okay);
   batch[0].Value = 1;
   AUDIO_REQUIRE(std::get<double>(set.Commands[0].Data) IS 0.25); // Admission owns a copy.
   AUDIO_REQUIRE(set.Commands[1].Handle IS handle + 1);
   AUDIO_REQUIRE(set.Commands[2].CommandID IS CMD::END_SEQUENCE);

   // Invalid suffixes must not publish an otherwise valid prefix.
   batch[1].Handle = 2 << 16;
   AUDIO_REQUIRE(submit_audio_batch(set, 1, batch) IS ERR::OutOfRange);
   batch[1].Handle = handle + 2;
   AUDIO_REQUIRE(submit_audio_batch(set, 1, batch) IS ERR::OutOfRange);
   batch[1].Handle = handle;
   batch[1].Operation = MIX(999);
   AUDIO_REQUIRE(submit_audio_batch(set, 1, batch) IS ERR::Args);
   batch[1].Operation = MIX::PAN;
   batch[1].Value = std::numeric_limits<double>::quiet_NaN();
   AUDIO_REQUIRE(submit_audio_batch(set, 1, batch) IS ERR::OutOfRange);
   AUDIO_REQUIRE(set.Commands.size() IS 3);
   batch[1].Value = 0;

   std::vector<AudioMixCommand> full(1023, batch[0]);
   AUDIO_REQUIRE(submit_audio_batch(set, 1, full) IS ERR::BufferOverflow);
   AUDIO_REQUIRE(set.Commands.size() IS 3);
   set.Commands.clear();
   AUDIO_REQUIRE(submit_audio_batch(set, 1, full) IS ERR::Okay);
   AUDIO_REQUIRE(set.Commands.size() IS 1024);
   AUDIO_REQUIRE(submit_audio_batch(set, 1, batch) IS ERR::BufferOverflow);
   AUDIO_REQUIRE(set.Commands.back().CommandID IS CMD::END_SEQUENCE);
   full.push_back(batch[0]);
   set.Commands.clear();
   AUDIO_REQUIRE(submit_audio_batch(set, 1, full) IS ERR::BufferOverflow);
   AUDIO_REQUIRE(submit_audio_batch(set, 1, {}) IS ERR::NullArgs);
   AUDIO_REQUIRE(set.Commands.empty());

   // Playback positions retain their full 64-bit value through batch staging.
   constexpr int64_t large_position = int64_t(INT32_MAX) + 4097;
   std::array<AudioMixCommand, 1> play = {{{ MIX::PLAY, handle, large_position, 0 }}};
   AUDIO_REQUIRE(submit_audio_batch(set, 1, play) IS ERR::Okay);
   AUDIO_REQUIRE(std::get<int64_t>(set.Commands.front().Data) IS large_position);
   set.Commands.clear();

   // One boundary consumes only the first batch, preserving cross-channel array order and later batches.
   batch[1].Handle = handle + 1;
   AUDIO_REQUIRE(submit_audio_batch(set, 1, batch) IS ERR::Okay);
   AUDIO_REQUIRE(submit_audio_batch(set, 1, batch) IS ERR::Okay);
   std::vector<int> applied;
   auto apply = [&](const AudioCommand &Command) { applied.push_back(Command.Handle); };
   execute_next_audio_batch(set, apply);
   AUDIO_REQUIRE(applied.size() IS 2 and applied[0] IS handle and applied[1] IS handle + 1);
   AUDIO_REQUIRE(set.Commands.size() IS 3);
   execute_next_audio_batch(set, apply);
   AUDIO_REQUIRE(applied.size() IS 4 and set.Commands.empty());
   execute_next_audio_batch(set, apply);
   AUDIO_REQUIRE(applied.size() IS 4);

   // Documented frame counts, including odd-frame ties and the minimum non-zero tick.
   set.Tempo = 125;
   AUDIO_REQUIRE(set.TickFrames(22050) IS SAMPLE(442));
   AUDIO_REQUIRE(set.TickFrames(44100) IS SAMPLE(882));
   AUDIO_REQUIRE(set.TickFrames(48000) IS SAMPLE(960));
   set.Tempo = 128;
   AUDIO_REQUIRE(set.TickFrames(44100) IS SAMPLE(862));
   set.Tempo = 1;
   AUDIO_REQUIRE(set.TickFrames(192000) IS SAMPLE(480000));
   set.Tempo = 100000;
   AUDIO_REQUIRE(set.TickFrames(8000) IS SAMPLE(2));
   AUDIO_REQUIRE(set.TickFrames(192000) IS SAMPLE(4));

   // A single command during playback preserves the current tick's remaining frames.
   set.Tempo = 125;
   set.MixLeft = set.TickFrames(44100);
   auto apply_tempo = [&](const AudioCommand &Command) {
      if (Command.CommandID IS CMD::TEMPO) set.Tempo = std::get<int>(Command.Data);
      else applied.push_back(Command.Handle);
   };
   advance_audio_tick(set, SAMPLE(400), 44100, apply_tempo);
   AUDIO_REQUIRE(set.MixLeft IS SAMPLE(482));
   set.Tempo = 250;
   advance_audio_tick(set, SAMPLE(481), 44100, apply_tempo);
   AUDIO_REQUIRE(set.MixLeft IS SAMPLE(1));
   advance_audio_tick(set, SAMPLE(1), 44100, apply_tempo);
   AUDIO_REQUIRE(set.MixLeft IS SAMPLE(442));

   // A batch tempo change schedules the immediately following tick; the last tempo wins.
   batch = {{{ MIX::TEMPO, handle, 125, 0 }, { MIX::TEMPO, handle + 1, 128, 0 }}};
   AUDIO_REQUIRE(submit_audio_batch(set, 1, batch) IS ERR::Okay);
   std::array<AudioMixCommand, 1> following = {{{ MIX::STOP, handle, 0, 0 }}};
   AUDIO_REQUIRE(submit_audio_batch(set, 1, following) IS ERR::Okay);
   applied.clear();
   advance_audio_tick(set, SAMPLE(442), 44100, apply_tempo);
   AUDIO_REQUIRE(set.Tempo IS 128 and set.MixLeft IS SAMPLE(862));
   AUDIO_REQUIRE(applied.empty() and set.Commands.size() IS 2);
   advance_audio_tick(set, SAMPLE(861), 44100, apply_tempo);
   AUDIO_REQUIRE(applied.empty());
   advance_audio_tick(set, SAMPLE(1), 44100, apply_tempo);
   AUDIO_REQUIRE(applied.size() IS 1 and set.Commands.empty());
}

} // namespace audio_tests_audio_batches
