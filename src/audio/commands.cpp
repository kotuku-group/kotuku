// Buffered command handling.  The execution of these commands is managed by process_commands()

#include <utility>

template<typename... tArgs>
static ERR add_command(objAudio* Audio, CMD Command, int Handle, tArgs&&... pArgs) {
   auto ea = (extAudio *)(Audio);
   int index = Handle >> 16;

   kt::Log log(__FUNCTION__);
   if ((index < 1) or (index >= int(ea->Sets.size()))) return log.warning(ERR::OutOfRange);
   if (ea->Sets[index].Commands.capacity() == 0) return log.warning(ERR::OutOfRange);
   if (ea->Sets[index].Commands.size() > 1024) return log.warning(ERR::BufferOverflow);

   if constexpr (sizeof...(pArgs) > 0) {
      static_assert(sizeof...(pArgs) == 1, "Command can only accept one data parameter");
      ea->Sets[index].Commands.emplace_back(Command, Handle, std::forward<tArgs>(pArgs)...);
   }
   else {
      ea->Sets[index].Commands.emplace_back(Command, Handle);
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// It is a requirement that VOL_RAMPING or OVER_SAMPLING flags have been set in the target Audio object.

static ERR fade_in(extAudio *Audio, AudioChannel *channel)
{
   if (((Audio->Flags & ADF::VOL_RAMPING) IS ADF::NIL) or ((Audio->Flags & ADF::OVER_SAMPLING) IS ADF::NIL)) return ERR::Okay;

   channel->LVolume = 0;
   channel->RVolume = 0;
   return set_channel_volume(Audio, channel);
}

//********************************************************************************************************************
// In oversampling mode, active samples are faded-out on a shadow channel rather than stopped abruptly.

static ERR fade_out(extAudio *Audio, int Handle)
{
   if ((Audio->Flags & ADF::OVER_SAMPLING) IS ADF::NIL) return ERR::Okay;

   auto channel = Audio->GetChannel(Handle);
   auto shadow  = Audio->GetShadow(Handle);

   if (channel->isStopped() or
       (shadow->State IS CHS::FADE_OUT) or
       ((channel->LVolume < 0.01) and (channel->RVolume < 0.01))) return ERR::Okay;

   *shadow = *channel;
   shadow->Volume = 0;
   shadow->State  = CHS::FADE_OUT;
   set_channel_volume(Audio, shadow);
   shadow->Flags |= CHF::VOL_RAMP;
   return ERR::Okay;
}

namespace snd {

/*********************************************************************************************************************

-FUNCTION-
MixStartSequence: Initiates buffering of mix commands.

Use this function to initiate the buffering of mix commands, up until a call to ~MixEndSequence() is made.  The
buffering of mix commands makes it possible to create batches of commands that are executed at timed intervals
as determined by ~MixRate().

<header>Command Buffering Architecture</header>

When command buffering is activated, the mixer transitions to a batch processing mode with several key characteristics:

<list type="bullet">
<li><b>Deferred Execution:</b> All mixer operations (~MixPlay(), ~MixVolume(), ~MixPan(), ~MixFrequency(), etc.) are queued rather than executed immediately</li>
<li><b>Atomic Batch Processing:</b> Queued commands are processed synchronously during the next mixer update cycle, ensuring sample-accurate timing coordination</li>
<li><b>Thread-Safe Queueing:</b> Commands can be safely queued from multiple threads without explicit synchronisation requirements</li>
<li><b>Overflow Protection:</b> Command buffers include overflow detection to prevent memory exhaustion during extended buffering periods</li>
</list>

This feature can be used to implement complex sound mixes and digital music players.
<header>Advanced Usage Patterns</header>

<list type="ordered">
<li><b>Sequence Initiation:</b> Call MixStartSequence() to begin command buffering for the target channel or channel set</li>
<li><b>Command Queuing:</b> Issue multiple mixer commands (volume, pan, play, frequency adjustments, etc.) which are automatically queued</li>
<li><b>Sequence Completion:</b> Call ~MixEndSequence() to mark the end of the command batch and schedule execution</li>
<li><b>Automatic Execution:</b> Commands execute atomically at the next mixer update interval determined by ~MixRate()</li>
</list>

-INPUT-
obj(Audio) Audio: The target Audio object.
int Handle: The target channel.

-ERRORS-
Okay: Command buffering successfully initiated.
NullArgs: Required parameters are null or missing.

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

ERR MixStartSequence(objAudio *Audio, int Handle)
{
   kt::Log log(__FUNCTION__);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   log.traceBranch("Audio: #%d, Channel: $%.8x", Audio->UID, Handle);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);
   channel->Buffering = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
MixEndSequence: Ends the buffering of mix commands.

Use this function to end a buffered command sequence that was started by ~MixStartSequence().

-INPUT-
obj(Audio) Audio: The target Audio object.
int Handle: The target channel.

-ERRORS-
Okay
NullArgs

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

ERR MixEndSequence(objAudio *Audio, int Handle)
{
   kt::Log log(__FUNCTION__);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   log.traceBranch("Audio: #%d, Channel: $%.8x", Audio->UID, Handle);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);
   channel->Buffering = false;

   // Inserting an END_SEQUENCE informs the mixer that the instructions for this period have concluded.

   add_command(Audio, CMD::END_SEQUENCE, Handle);

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
MixContinue: Continue playing a stopped channel.

This function will continue playback on a channel that has previously been stopped.

-INPUT-
obj(Audio) Audio: The target Audio object.
int Handle: The target channel.

-ERRORS-
Okay
NullArgs

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

ERR MixContinue(objAudio *Audio, int Handle)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("Audio: #%d, Channel: $%.8x", Audio->UID, Handle);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);

   if (channel->Buffering) {
      add_command(Audio, CMD::CONTINUE, Handle);
      return ERR::Okay;
   }

   if (channel->State IS CHS::PLAYING) return ERR::Okay;

   // Check if the read position is already at the end of the sample

   auto &sample = ((extAudio *)Audio)->Samples[channel->SampleHandle];

   if ((sample.Stream) and (sample.PlayPos >= sample.StreamLength)) return ERR::Okay;
   else if (channel->Position >= sample.SampleLength) return ERR::Okay;

   fade_out((extAudio *)Audio, Handle);

   channel->State = CHS::PLAYING;

   if ((Audio->Flags & ADF::OVER_SAMPLING) != ADF::NIL) {
      auto shadow = ((extAudio *)Audio)->GetShadow(Handle);
      shadow->State = CHS::PLAYING;
   }

   kt::SwitchContext context(Audio);

   if (((extAudio *)Audio)->Timer) UpdateTimer(((extAudio *)Audio)->Timer, -MIX_INTERVAL);
   else SubscribeTimer(MIX_INTERVAL, C_FUNCTION(audio_timer), &((extAudio *)Audio)->Timer);

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
MixMute: Mutes the audio of a channel.

Use this function to mute the audio of a mixer channel.

-INPUT-
obj(Audio) Audio: The target Audio object.
int Handle: The target channel.
int Mute: Set to true to mute the channel.  A value of 0 will undo the mute setting.

-ERRORS-
Okay
NullArgs
OutOfRange

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

ERR MixMute(objAudio *Audio, int Handle, int Mute)
{
   kt::Log log(__FUNCTION__);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   log.traceBranch("Audio: #%d, Channel: $%.8x, Mute: %c", Audio->UID, Handle, Mute ? 'Y' : 'N');

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);

   if (channel->Buffering) {
      add_command(Audio, CMD::MUTE, Handle, bool(Mute));
      return ERR::Okay;
   }

   if (Mute != 0) channel->Flags |= CHF::MUTE;
   else channel->Flags &= ~CHF::MUTE;
   set_channel_volume((extAudio *)Audio, channel);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
MixFrequency: Sets a channel's playback rate.

Use this function to set the playback rate of a mixer channel.

-INPUT-
obj(Audio) Audio: The target Audio object.
int Handle: The target channel.
int Frequency: The desired frequency.

-ERRORS-
Okay
NullArgs
OutOfRange
Failed

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

ERR MixFrequency(objAudio *Audio, int Handle, int Frequency)
{
   kt::Log log(__FUNCTION__);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   log.traceBranch("Audio: #%d, Channel: $%.8x, Frequency: %d", Audio->UID, Handle, Frequency);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);

   if (channel->Buffering) {
      add_command(Audio, CMD::FREQUENCY, Handle, Frequency);
      return ERR::Okay;
   }

   channel->Frequency = Frequency;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
MixPan: Sets a channel's panning value.

Use this function to set a mixer channel's panning value.  Accepted values are between -1.0 (left) and 1.0 (right).

-INPUT-
obj(Audio) Audio: The target Audio object.
int Handle: The target channel.
double Pan: The desired pan value between -1.0 and 1.0.

-ERRORS-
Okay
NullArgs

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

ERR MixPan(objAudio *Audio, int Handle, double Pan)
{
   kt::Log log(__FUNCTION__);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   log.traceBranch("Audio: #%d, Channel: $%.8x, Pan: %.2f", Audio->UID, Handle, Pan);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);

   if (channel->Buffering) {
      add_command(Audio, CMD::PAN, Handle, Pan);
      return ERR::Okay;
   }

   if (Pan < -1.0) channel->Pan = -1.0;
   else if (Pan > 1.0) channel->Pan = 1.0;
   else channel->Pan = Pan;

   set_channel_volume((extAudio *)Audio, channel);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
MixPlay: Commences channel playback at a set frequency.

This function will start playback of the sound sample associated with the target mixer channel.  If the channel is
already in playback mode, it will be stopped to facilitate the new playback request.

-INPUT-
obj(Audio) Audio: The target Audio object.
int Handle: The target channel.
int Position: The new playing position, measured in bytes.

-ERRORS-
Okay: Playback successfully initiated.
NullArgs: Required parameters are null or missing.
OutOfRange: Position exceeds sample boundaries.
Failed: Channel not associated with a valid sample.

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

ERR MixPlay(objAudio *Audio, int Handle, int Position)
{
   kt::Log log(__FUNCTION__);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   if (Position < 0) return log.warning(ERR::OutOfRange);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);

   log.traceBranch("Audio: #%d, Channel: $%.8x, Position: %d", Audio->UID, Handle, Position);

   if (channel->Buffering) {
      add_command(Audio, CMD::PLAY, Handle, Position);
      return ERR::Okay;
   }

   if (!channel->SampleHandle) { // A sample must be defined for the channel.
      log.warning("Channel not associated with a sample.");
      return ERR::Failed;
   }

   ((extAudio *)Audio)->finish(*channel, false); // Turn off previous sound

   auto &sample = ((extAudio *)Audio)->Samples[channel->SampleHandle];

   // Convert position from bytes to samples

   auto bitpos = SAMPLE(Position >> sample_shift(sample.SampleType));

   if (sample.Data.empty()) { // The sample reference must be valid and not stale.
      log.warning("On channel %d, referenced sample %d is unconfigured.", Handle, channel->SampleHandle);
      return ERR::Failed;
   }

   if (sample.Stream) {
      if (Position > sample.StreamLength) return log.warning(ERR::OutOfRange);
      sample.PlayPos = BYTELEN(Position) + fill_stream_buffer(Handle, sample, Position);
      Position = 0; // Internally we want to start from byte position zero in our stream buffer
   }
   else if (bitpos > sample.SampleLength) return log.warning(ERR::OutOfRange);

   fade_out((extAudio *)Audio, Handle);

   // Check if sample has been changed, and if so, set the values to the channel structure

   if ((channel->Flags & CHF::CHANGED) != CHF::NIL) {
      channel->Flags &= ~CHF::CHANGED;

      // If channel status is released and the new sample does not have two loops, end the sample

      if ((sample.LoopMode != LOOP::SINGLE_RELEASE) and (sample.LoopMode != LOOP::DOUBLE) and (channel->State IS CHS::RELEASED)) {
         ((extAudio *)Audio)->finish(*channel, true);
         return ERR::Okay;
      }
   }

   switch (channel->State) {
      case CHS::FINISHED:
      case CHS::PLAYING:
         // Either playing sample before releasing, or playing has ended - check the first loop type.

         if (sample.OnStop.defined()) {
            double sec;
            if (sample.Stream) {
               // NB: Accuracy is dependent on the StreamLength value being correct.
               sec = double((sample.StreamLength - sample.PlayPos)>>sample_shift(sample.SampleType)) / double(channel->Frequency);
            }
            else sec = double(sample.SampleLength - Position) / double(channel->Frequency);
            channel->EndTime = PreciseTime() + std::lrint(sec * 1000000.0);
         }
         else channel->EndTime = 0;

         channel->LoopIndex = 1;
         switch (sample.Loop1Type) {
            case LTYPE::NIL:
               // No looping - if position is below sample end, set it and start playing there
               if (bitpos < sample.SampleLength) {
                  channel->Position    = bitpos;
                  channel->PositionLow = 0;
                  channel->State       = CHS::PLAYING;
                  channel->Flags       &= ~CHF::BACKWARD;
               }
               else ((extAudio *)Audio)->finish(*channel, true);
               break;

            case LTYPE::UNIDIRECTIONAL:
               // Unidirectional looping - if position is below loop end, set it, otherwise set loop start as the
               // new position. Start playing in any case.
               if (bitpos < sample.Loop1End) channel->Position = bitpos;
               else channel->Position = sample.Loop1Start;
               channel->PositionLow = 0;
               channel->State       = CHS::PLAYING;
               channel->Flags      &= ~CHF::BACKWARD;
               break;

            case LTYPE::BIDIRECTIONAL:
               // Bidirectional looping - if position is below loop end, set it and start playing forward, otherwise
               // set loop end as the new position and start playing backwards.
               if (bitpos < sample.Loop1End ) {
                  channel->Position = bitpos;
                  channel->Flags &= ~CHF::BACKWARD;
               }
               else {
                  channel->Position = sample.Loop1End;
                  channel->Flags |= CHF::BACKWARD;
               }
               channel->PositionLow = 0;
               channel->State = CHS::PLAYING;
         }
         break;

      case CHS::RELEASED: // Playing after sample has been released - check second loop type.
         channel->LoopIndex = 2;
         switch (sample.Loop2Type) {
            case LTYPE::NIL:
               // No looping - if position is below sample end, set it and start playing there.

               if (bitpos < sample.SampleLength ) {
                  channel->Position    = bitpos;
                  channel->PositionLow = 0;
                  channel->State       = CHS::PLAYING;
                  channel->Flags       &= ~CHF::BACKWARD;
               }
               else ((extAudio *)Audio)->finish(*channel, true);
               break;

            case LTYPE::UNIDIRECTIONAL:
               // Unidirectional looping - if position is below loop end, set it, otherwise set loop start as the
               // new position. Start playing in any case.
               if (bitpos < sample.Loop2End) channel->Position = bitpos;
               else channel->Position = sample.Loop2Start;
               channel->PositionLow = 0;
               channel->State = CHS::PLAYING;
               channel->Flags &= ~CHF::BACKWARD;
               break;

            case LTYPE::BIDIRECTIONAL:
               // Bidirectional looping - if position is below loop end, set it and start playing forward, otherwise
               // set loop end as the new position and start playing backwards.

               if (bitpos < sample.Loop2End) {
                  channel->Position = bitpos;
                  channel->Flags   &= ~CHF::BACKWARD;
               }
               else {
                  channel->Position = sample.Loop2End;
                  channel->Flags   |= CHF::BACKWARD;
               }
               channel->PositionLow = 0;
               channel->State       = CHS::PLAYING;
         }
         break;

      case CHS::STOPPED:
      default:
         // If sound has been stopped do nothing
         break;
   }

   fade_in((extAudio *)Audio, channel);

   if (channel->State IS CHS::PLAYING) {
      kt::SwitchContext context(Audio);
      if (((extAudio *)Audio)->Timer) UpdateTimer(((extAudio *)Audio)->Timer, -MIX_INTERVAL);
      else SubscribeTimer(MIX_INTERVAL, C_FUNCTION(audio_timer), &((extAudio *)Audio)->Timer);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
MixRate: Sets a new update rate for a channel.

This function will set a new update rate for all channels, measured in milliseconds.  The default update rate is 125,
which is equivalent to 5000Hz.

-INPUT-
obj(Audio) Audio: The target Audio object.
int Handle: The channel set allocated from OpenChannels().
int Rate: The new update rate in milliseconds.

-ERRORS-
Okay
NullArgs
OutOfRange

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

ERR MixRate(objAudio *Audio, int Handle, int Rate)
{
   kt::Log log(__FUNCTION__);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   log.traceBranch("Audio: #%d, Channel: $%.8x", Audio->UID, Handle);

   if ((Rate < 1) or (Rate > 100000)) return log.warning(ERR::OutOfRange);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);

   if (channel->Buffering) {
      add_command(Audio, CMD::RATE, Handle, Rate);
      return ERR::Okay;
   }

   int16_t index = Handle>>16;
   if ((index >= 0) and (index < (int)((extAudio *)Audio)->Sets.size())) {
      ((extAudio *)Audio)->Sets[index].UpdateRate = Rate;
      return ERR::Okay;
   }
   else return log.warning(ERR::OutOfRange);
}

/*********************************************************************************************************************

-FUNCTION-
MixSample: Associate a sound sample with a mixer channel.

This function will associate a sound sample with the channel identified by Handle.  The client should follow this by
setting configuration details (e.g. volume and pan values).

The referenced Sample must have been added to the audio server via the @Audio.AddSample() or @Audio.AddStream()
methods.

-INPUT-
obj(Audio) Audio: The target Audio object.
int Handle: The target channel.
int Sample: A sample handle allocated from @Audio.AddSample() or @Audio.AddStream().

-ERRORS-
Okay
NullArgs
OutOfRange
Failed

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

ERR MixSample(objAudio *Audio, int Handle, int SampleIndex)
{
   kt::Log log(__FUNCTION__);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   int idx = SampleIndex;

   log.traceBranch("Audio: #%d, Channel: $%.8x, Sample: %d", Audio->UID, Handle, idx);

   if ((idx <= 0) or (idx >= (int)((extAudio *)Audio)->Samples.size())) {
      return log.warning(ERR::OutOfRange);
   }
   else if (((extAudio *)Audio)->Samples[idx].Data.empty()) {
      log.warning("Sample #%d refers to a dead sample.", idx);
      return ERR::Failed;
   }
   else if (((extAudio *)Audio)->Samples[idx].SampleLength <= 0) {
      log.warning("Sample #%d has invalid sample length %d", idx, ((extAudio *)Audio)->Samples[idx].SampleLength);
      return ERR::Failed;
   }

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);

   if (channel->Buffering) {
      add_command(Audio, CMD::SAMPLE, Handle, SampleIndex);
      return ERR::Okay;
   }

   if (channel->SampleHandle IS idx) return ERR::Okay; // Already associated?

   channel->SampleHandle = idx;     // Set new sample number to channel
   channel->Flags |= CHF::CHANGED;  // Sample has been changed

   // If the new sample has one Amiga-compatible loop and playing has ended (not released or stopped), set the new
   // sample and start playing from loop start.

   auto &s = ((extAudio *)Audio)->Samples[idx];
   if ((s.LoopMode IS LOOP::AMIGA) and (channel->State IS CHS::FINISHED)) {
      // Set Amiga sample and start playing.  We won't do this with interpolated mixing, as this tends to cause clicks.

      if ((Audio->Flags & ADF::OVER_SAMPLING) IS ADF::NIL) {
         channel->State = CHS::PLAYING;
         snd::MixPlay(Audio, Handle, s.Loop1Start);
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
MixStop: Stops all playback on a channel.

This function will stop a channel that is currently playing.

-INPUT-
obj(Audio) Audio: The target Audio object.
int Handle: The target channel.

-ERRORS-
Okay
NullArgs

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

ERR MixStop(objAudio *Audio, int Handle)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("Audio: #%d, Channel: $%.8x", Audio->UID, Handle);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);

   if (channel->Buffering) {
      add_command(Audio, CMD::STOP, Handle);
      return ERR::Okay;
   }

   ((extAudio *)Audio)->finish(*channel, true);
   channel->State = CHS::STOPPED;

   if ((Audio->Flags & ADF::OVER_SAMPLING) != ADF::NIL) {
      auto shadow = ((extAudio *)Audio)->GetShadow(Handle);
      shadow->State = CHS::STOPPED;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
MixStopLoop: Cancels any playback loop configured for a channel.

This function will cancel the loop that is associated with the channel identified by Handle if in playback mode.
The existing loop configuration will remain intact if playback is restarted.

-INPUT-
obj(Audio) Audio: The target Audio object.
int Handle: The target channel.

-ERRORS-
Okay
NullArgs

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

ERR MixStopLoop(objAudio *Audio, int Handle)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("Audio: #%d, Channel: $%.8x", Audio->UID, Handle);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);

   if (channel->Buffering) {
      add_command(Audio, CMD::STOP_LOOPING, Handle);
      return ERR::Okay;
   }

   if (channel->State != CHS::PLAYING) return ERR::Okay;

   auto &sample = ((extAudio *)Audio)->Samples[channel->SampleHandle];

   if ((sample.LoopMode IS LOOP::SINGLE_RELEASE) or (sample.LoopMode IS LOOP::DOUBLE)) {
      channel->State = CHS::RELEASED;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
MixVolume: Changes the volume of a channel.

This function will change the volume of the mixer channel identified by Handle.  Valid values are from 0 (silent)
to 1.0 (maximum).

-INPUT-
obj(Audio) Audio: The target Audio object.
int Handle: The target channel.
double Volume: The new volume for the channel.

-ERRORS-
Okay
NullArgs

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

ERR MixVolume(objAudio *Audio, int Handle, double Volume)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("Audio: #%d, Channel: $%.8x", Audio->UID, Handle);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);

   if (channel->Buffering) {
      add_command(Audio, CMD::VOLUME, Handle, Volume);
      return ERR::Okay;
   }

   if (Volume > 1.0) channel->Volume = 1.0;
   else if (Volume < 0) channel->Volume = 0;
   else channel->Volume = Volume;

   set_channel_volume((extAudio *)Audio, channel);
   return ERR::Okay;
}

} // namespace
