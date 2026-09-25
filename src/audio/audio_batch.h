#pragma once

#include <cmath>

// Caller holds MixerMutex.  Stage and validate everything before publishing the batch and its boundary.
static ERR submit_audio_batch(ChannelSet &Set, unsigned Index, std::span<const AudioMixCommand> Commands)
{
   if (Commands.empty()) return ERR::NullArgs;
   if (Commands.size() > 1023 or Commands.size() + 1 > 1024 - Set.Commands.size()) {
      return ERR::BufferOverflow;
   }

   std::array<AudioCommand, 1023> staged;
   size_t count = 0;
   for (const auto &command : Commands) {
      const int handle = command.Handle;
      if ((unsigned(handle) >> 16) != Index or size_t(handle & 0xffff) >= Set.Channel.size()) {
         return ERR::OutOfRange;
      }
      const int value = command.Integer;
      switch (command.Operation) {
         case MIX::CONTINUE: staged[count++] = AudioCommand(CMD::CONTINUE, handle); break;
         case MIX::STOP: staged[count++] = AudioCommand(CMD::STOP, handle); break;
         case MIX::STOP_LOOP: staged[count++] = AudioCommand(CMD::STOP_LOOPING, handle); break;
         case MIX::MUTE: staged[count++] = AudioCommand(CMD::MUTE, handle, bool(value)); break;
         case MIX::FREQUENCY:
            if (value < 0 or value > 192000) return ERR::OutOfRange;
            staged[count++] = AudioCommand(CMD::FREQUENCY, handle, value);
            break;
         case MIX::PLAY:
            if (value < 0) return ERR::OutOfRange;
            staged[count++] = AudioCommand(CMD::PLAY, handle, value);
            break;
         case MIX::TEMPO:
            if (value < 1 or value > 100000) return ERR::OutOfRange;
            staged[count++] = AudioCommand(CMD::TEMPO, handle, value);
            break;
         case MIX::SAMPLE:
            if (value <= 0) return ERR::OutOfRange;
            staged[count++] = AudioCommand(CMD::SAMPLE, handle, value);
            break;
         case MIX::PAN:
         case MIX::VOLUME:
            if (!std::isfinite(command.Value)) return ERR::OutOfRange;
            staged[count++] = AudioCommand(command.Operation IS MIX::PAN ? CMD::PAN : CMD::VOLUME,
               handle, command.Value);
            break;
         default: return ERR::Args;
      }
   }
   Set.Commands.insert(Set.Commands.end(), staged.begin(), staged.begin() + count);
   Set.Commands.emplace_back(CMD::END_SEQUENCE, Commands.front().Handle);
   return ERR::Okay;
}

// Run exactly one admitted batch at an update boundary.  The callback applies a command without rendering frames.
template<typename ApplyCommand>
static void execute_next_audio_batch(ChannelSet &Set, ApplyCommand Apply)
{
   size_t count = 0;
   while (count < Set.Commands.size()) {
      const auto &command = Set.Commands[count++];
      if (command.CommandID IS CMD::END_SEQUENCE) break;
      Apply(command);
   }
   Set.Commands.erase(Set.Commands.begin(), Set.Commands.begin() + count);
}

// Rendering stops at each boundary.  Apply its batch before scheduling the following tick.
template<typename ApplyCommand>
static void advance_audio_tick(ChannelSet &Set, SAMPLE Frames, int OutputRate, ApplyCommand Apply)
{
   if (Set.Channel.empty()) return;
   Set.MixLeft -= Frames;
   if (Set.MixLeft > 0) return;
   execute_next_audio_batch(Set, Apply);
   Set.MixLeft = Set.TickFrames(OutputRate);
}
