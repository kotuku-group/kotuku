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
   if (Audio->Samples[channel->SampleHandle].Stream) return ERR::Okay;
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

static ERR pause_channel(objAudio *Audio, int Handle);

#ifdef AUDIO_WORKER
// A queued MixSample can precede MixPlay without having reached the mixer yet.
static int queued_sample_handle(extAudio *Self, int Handle, int Current)
{
   for (size_t i = Self->PendingCount; i > 0; --i) {
      const auto &command = Self->PendingCommands[i - 1];
      if (command.Handle IS Handle and command.CommandID IS CMD::SAMPLE) return std::get<int>(command.Data);
   }
   return Current;
}
#endif

/*********************************************************************************************************************

-FUNCTION-
MixSubmitBatch: Submits an independently owned batch of mixer commands.

This function submits a list of mixer commands in a single batch.  `Commands` is an array of !AudioMixCommand
records.  Submission takes a copy of the complete array, or rejects it without publishing any commands.

All commands must address live channels in the same channel set.  At most 1023 commands can be submitted at once;
the set's queue holds 1024 entries, including one internal boundary entry per batch.  Invalid command types,
channels or numeric arguments reject the entire batch.  Sample availability and playback state are checked at
execution time.

Batches execute in submission order within their set, with one batch per mixer update boundary as determined by
~MixRate().  Commands execute in array order without rendering frames.  Different sets have independent update
boundaries.  On worker backends, individual Mix calls use a separate FIFO which is drained before rendering each
period (and by synchronous Sound queries); they can overtake queued batches, even when submitted later.  On backends
without a worker, individual Mix calls execute immediately.

Successful submission means queue admission, not successful playback or audible output.  A failed command does not
roll back earlier commands or prevent later commands in the batch from running.  Closing the channel set discards
its queued batches.  Existing sample lifetime rules apply; keep referenced samples alive until execution.

-INPUT-
obj(Audio) Audio: The target Audio object.
array(struct(AudioMixCommand)) Commands: Commands to copy into one channel set's queue.

-ERRORS-
Okay
NullArgs
OutOfRange
Args
BufferOverflow
NotInitialised

-TAGS-
mutates-object, copies-input
-END-

*********************************************************************************************************************/

ERR MixSubmitBatch(objAudio *Audio, const std::span<const AudioMixCommand> &Commands)
{
   if (!Audio or Commands.empty()) return ERR::NullArgs;
   auto self = (extAudio *)Audio;
   std::lock_guard mixer_lock(self->MixerMutex);

#ifdef AUDIO_WORKER
   if (self->StopWorker) return ERR::NotInitialised;
#endif

   const auto index = unsigned(Commands.front().Handle) >> 16;
   if (!index or index >= self->Sets.size()) return ERR::OutOfRange;
   auto result = submit_audio_batch(self->Sets[index], index, Commands);

#ifdef AUDIO_WORKER
   if (result IS ERR::Okay) wake_audio(self);
#endif
   return result;
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
   if (!Audio or !Handle) return ERR::NullArgs;
   std::lock_guard mixer_lock(((extAudio *)Audio)->MixerMutex);
   if (!((extAudio *)Audio)->GetChannel(Handle)) return ERR::OutOfRange;

#ifdef AUDIO_WORKER
   if (!glAudioWorker) {
      auto self = (extAudio *)Audio;
      if (self->StopWorker) return ERR::NotInitialised;
      if (self->PendingCount >= self->PendingCommands.size()) return ERR::BufferOverflow;
      self->PendingCommands[self->PendingCount++] = AudioCommand(CMD::CONTINUE, Handle);
      wake_audio(self);
      return ERR::Okay;
   }
#endif

   AudioLog log(__FUNCTION__);

   log.traceBranch("Audio: #%d, Channel: $%.8x", Audio->UID, Handle);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);

   if (channel->State IS CHS::PLAYING) return ERR::Okay;

   // Check if the read position is already at the end of the sample

   auto &sample = ((extAudio *)Audio)->Samples[channel->SampleHandle];

   if ((sample.Stream) and (sample.PlayPos >= sample.StreamLength)) return ERR::Okay;
   else if (channel->Position >= sample.SampleLength) return ERR::Okay;

   fade_out((extAudio *)Audio, Handle);

   ++channel->PlaybackGeneration;
   channel->State = CHS::PLAYING;

   if ((Audio->Flags & ADF::OVER_SAMPLING) != ADF::NIL) {
      auto shadow = ((extAudio *)Audio)->GetShadow(Handle);
      shadow->State = CHS::PLAYING;
   }


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
   if (!Audio or !Handle) return ERR::NullArgs;
   std::lock_guard mixer_lock(((extAudio *)Audio)->MixerMutex);
   if (!((extAudio *)Audio)->GetChannel(Handle)) return ERR::OutOfRange;

#ifdef AUDIO_WORKER
   if (!glAudioWorker) {
      auto self = (extAudio *)Audio;
      if (self->StopWorker) return ERR::NotInitialised;
      if (self->PendingCount >= self->PendingCommands.size()) return ERR::BufferOverflow;
      self->PendingCommands[self->PendingCount++] = AudioCommand(CMD::MUTE, Handle, bool(Mute));
      wake_audio(self);
      return ERR::Okay;
   }
#endif

   AudioLog log(__FUNCTION__);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   log.traceBranch("Audio: #%d, Channel: $%.8x, Mute: %c", Audio->UID, Handle, Mute ? 'Y' : 'N');

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);


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
   if (!Audio or !Handle) return ERR::NullArgs;
   std::lock_guard mixer_lock(((extAudio *)Audio)->MixerMutex);
   if (!((extAudio *)Audio)->GetChannel(Handle)) return ERR::OutOfRange;
   if (Frequency < 0 or Frequency > 192000) return ERR::OutOfRange;

#ifdef AUDIO_WORKER
   if (!glAudioWorker) {
      auto self = (extAudio *)Audio;
      if (self->StopWorker) return ERR::NotInitialised;
      if (self->PendingCount >= self->PendingCommands.size()) return ERR::BufferOverflow;
      self->PendingCommands[self->PendingCount++] = AudioCommand(CMD::FREQUENCY, Handle, Frequency);
      wake_audio(self);
      return ERR::Okay;
   }
#endif

   AudioLog log(__FUNCTION__);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   log.traceBranch("Audio: #%d, Channel: $%.8x, Frequency: %d", Audio->UID, Handle, Frequency);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);

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
   if (!Audio or !Handle) return ERR::NullArgs;
   std::lock_guard mixer_lock(((extAudio *)Audio)->MixerMutex);
   if (!((extAudio *)Audio)->GetChannel(Handle)) return ERR::OutOfRange;

#ifdef AUDIO_WORKER
   if (!glAudioWorker) {
      auto self = (extAudio *)Audio;
      if (self->StopWorker) return ERR::NotInitialised;
      if (self->PendingCount >= self->PendingCommands.size()) return ERR::BufferOverflow;
      self->PendingCommands[self->PendingCount++] = AudioCommand(CMD::PAN, Handle, Pan);
      wake_audio(self);
      return ERR::Okay;
   }
#endif

   AudioLog log(__FUNCTION__);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   log.traceBranch("Audio: #%d, Channel: $%.8x, Pan: %.2f", Audio->UID, Handle, Pan);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);

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
FieldNotSet: Channel not associated with a valid sample.
NoData: The referenced sample is unconfigured.

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

ERR MixPlay(objAudio *Audio, int Handle, int Position)
{
   if (!Audio or !Handle) return ERR::NullArgs;
   std::lock_guard mixer_lock(((extAudio *)Audio)->MixerMutex);
   if (!((extAudio *)Audio)->GetChannel(Handle)) return ERR::OutOfRange;
   if (Position < 0) return ERR::OutOfRange;

#ifdef AUDIO_WORKER
   if (!glAudioWorker) {
      auto self = (extAudio *)Audio;
      if (self->StopWorker) return ERR::NotInitialised;
      const int sample_handle = queued_sample_handle(self, Handle, self->GetChannel(Handle)->SampleHandle);
      if (!sample_handle) return ERR::FieldNotSet;
      if (sample_handle < 0 or size_t(sample_handle) >= self->Samples.size()) return ERR::NoData;
      const auto &sample = self->Samples[sample_handle];
      if (sample.Data.empty()) return ERR::NoData;
      if (sample.Stream) {
         if (Position > sample.StreamLength) return ERR::OutOfRange;
      }
      else if (SAMPLE(Position >> sample_shift(sample.SampleType)) > sample.SampleLength) return ERR::OutOfRange;
      if (self->PendingCount >= self->PendingCommands.size()) return ERR::BufferOverflow;
      self->PendingCommands[self->PendingCount++] = AudioCommand(CMD::PLAY, Handle, Position);
      wake_audio(self);
      return ERR::Okay;
   }
#endif

   AudioLog log(__FUNCTION__);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   if (Position < 0) return log.warning(ERR::OutOfRange);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);

   log.traceBranch("Audio: #%d, Channel: $%.8x, Position: %d", Audio->UID, Handle, Position);


   if (!channel->SampleHandle) { // A sample must be defined for the channel.
      log.warning("Channel not associated with a sample.");
      return ERR::FieldNotSet;
   }

   ((extAudio *)Audio)->finish(*channel, false); // Turn off previous sound

   auto &sample = ((extAudio *)Audio)->Samples[channel->SampleHandle];

   // Convert position from bytes to samples

   auto bitpos = SAMPLE(Position >> sample_shift(sample.SampleType));

   if (sample.Data.empty()) { // The sample reference must be valid and not stale.
      log.warning("On channel %d, referenced sample %d is unconfigured.", Handle, channel->SampleHandle);
      return ERR::NoData;
   }

   if (sample.Stream) {
      if (Position > sample.StreamLength) return log.warning(ERR::OutOfRange);
#ifdef AUDIO_WORKER
      ++sample.Generation;
      sample.DeferredStops = 0;
      sample.Ring.Read = sample.Ring.Used = 0;
      Position = (Position >> sample_shift(sample.SampleType)) << sample_shift(sample.SampleType);
      sample.SourceOffset = Position;
      sample.SourceSeek = true;
      sample.Refilling = false;
      sample.RefillPending = false;
      sample.RetryAt = 0;
      sample.PlayPos = BYTELEN(Position);
      sample.BufferedLength = BYTELEN(0);
      sample.Prefilled = sample.EndOfSource = sample.Starved = false;
      request_stream((extAudio *)Audio, sample);
#else
      sample.BufferedLength = fill_stream_buffer(Handle, sample, Position);
      sample.PlayPos = BYTELEN(Position) + sample.BufferedLength;
#endif
      Position = 0; // Internally we want to start from byte position zero in our stream buffer
      bitpos = SAMPLE(0);
   }
   else if (bitpos > sample.SampleLength) return log.warning(ERR::OutOfRange);

   fade_out((extAudio *)Audio, Handle);
   ++channel->PlaybackGeneration;

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
               // NB: Accuracy is dependent on the StreamLength value being correct.  PlayPos already includes the
               // buffered fill, which still has to be played, so it is added back to the anticipated time.
               sec = double((sample.StreamLength - sample.PlayPos + sample.BufferedLength)>>sample_shift(sample.SampleType)) / double(channel->Frequency);
            }
            else sec = double(sample.SampleLength - bitpos) / double(channel->Frequency);
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
   if (!Audio or !Handle) return ERR::NullArgs;
   std::lock_guard mixer_lock(((extAudio *)Audio)->MixerMutex);
   if (!((extAudio *)Audio)->GetChannel(Handle)) return ERR::OutOfRange;
   if (Rate < 1 or Rate > 100000) return ERR::OutOfRange;

#ifdef AUDIO_WORKER
   if (!glAudioWorker) {
      auto self = (extAudio *)Audio;
      if (self->StopWorker) return ERR::NotInitialised;
      if (self->PendingCount >= self->PendingCommands.size()) return ERR::BufferOverflow;
      self->PendingCommands[self->PendingCount++] = AudioCommand(CMD::RATE, Handle, Rate);
      wake_audio(self);
      return ERR::Okay;
   }
#endif

   AudioLog log(__FUNCTION__);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   log.traceBranch("Audio: #%d, Channel: $%.8x", Audio->UID, Handle);

   if ((Rate < 1) or (Rate > 100000)) return log.warning(ERR::OutOfRange);

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
NoData: The sample handle refers to a dead or unconfigured sample.
DataSize: The sample has an invalid length.

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

ERR MixSample(objAudio *Audio, int Handle, int SampleIndex)
{
   if (!Audio or !Handle) return ERR::NullArgs;
   std::lock_guard mixer_lock(((extAudio *)Audio)->MixerMutex);
   if (!((extAudio *)Audio)->GetChannel(Handle)) return ERR::OutOfRange;
   if (SampleIndex <= 0 or size_t(SampleIndex) >= ((extAudio *)Audio)->Samples.size()) return ERR::OutOfRange;

#ifdef AUDIO_WORKER
   if (!glAudioWorker) {
      auto self = (extAudio *)Audio;
      if (self->StopWorker) return ERR::NotInitialised;
      const auto &sample = self->Samples[SampleIndex];
      if (sample.Data.empty()) return ERR::NoData;
      if (sample.SampleLength <= 0) return ERR::DataSize;
      if (self->PendingCount >= self->PendingCommands.size()) return ERR::BufferOverflow;
      self->PendingCommands[self->PendingCount++] = AudioCommand(CMD::SAMPLE, Handle, SampleIndex);
      wake_audio(self);
      return ERR::Okay;
   }
#endif

   AudioLog log(__FUNCTION__);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   int idx = SampleIndex;

   log.traceBranch("Audio: #%d, Channel: $%.8x, Sample: %d", Audio->UID, Handle, idx);

   if ((idx <= 0) or (idx >= (int)((extAudio *)Audio)->Samples.size())) {
      return log.warning(ERR::OutOfRange);
   }
   else if (((extAudio *)Audio)->Samples[idx].Data.empty()) {
      log.warning("Sample #%d refers to a dead sample.", idx);
      return ERR::NoData;
   }
   else if (((extAudio *)Audio)->Samples[idx].SampleLength <= 0) {
      log.warning("Sample #%d has invalid sample length %d", idx, ((extAudio *)Audio)->Samples[idx].SampleLength);
      return ERR::DataSize;
   }

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);
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
   if (!Audio or !Handle) return ERR::NullArgs;
   std::lock_guard mixer_lock(((extAudio *)Audio)->MixerMutex);
   if (!((extAudio *)Audio)->GetChannel(Handle)) return ERR::OutOfRange;

#ifdef AUDIO_WORKER
   if (!glAudioWorker) {
      auto self = (extAudio *)Audio;
      if (self->StopWorker) return ERR::NotInitialised;
      if (self->PendingCount >= self->PendingCommands.size()) return ERR::BufferOverflow;
      self->PendingCommands[self->PendingCount++] = AudioCommand(CMD::STOP, Handle);
      wake_audio(self);
      return ERR::Okay;
   }
#endif

   AudioLog log(__FUNCTION__);

   log.traceBranch("Audio: #%d, Channel: $%.8x", Audio->UID, Handle);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);

   ((extAudio *)Audio)->finish(*channel, true);
   channel->State = CHS::STOPPED;

   if ((Audio->Flags & ADF::OVER_SAMPLING) != ADF::NIL) {
      auto shadow = ((extAudio *)Audio)->GetShadow(Handle);
      shadow->State = CHS::STOPPED;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Pause a channel without completing its current playback.  This is internal because the public mixer API uses
// MixStop() for terminal stops.

static ERR pause_channel(objAudio *Audio, int Handle)
{
   if (!Audio or !Handle) return ERR::NullArgs;
   auto self = (extAudio *)Audio;
   std::lock_guard mixer_lock(self->MixerMutex);
   auto channel = self->GetChannel(Handle);
   if (!channel) return ERR::OutOfRange;

#ifdef AUDIO_WORKER
   if (!glAudioWorker) {
      if (self->StopWorker) return ERR::NotInitialised;
      if (self->PendingCount >= self->PendingCommands.size()) return ERR::BufferOverflow;
      self->PendingCommands[self->PendingCount++] = AudioCommand(CMD::PAUSE, Handle);
      wake_audio(self);
      return ERR::Okay;
   }
#endif

   channel->State = CHS::STOPPED;
   if ((Audio->Flags & ADF::OVER_SAMPLING) != ADF::NIL) self->GetShadow(Handle)->State = CHS::STOPPED;
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
   if (!Audio or !Handle) return ERR::NullArgs;
   std::lock_guard mixer_lock(((extAudio *)Audio)->MixerMutex);
   if (!((extAudio *)Audio)->GetChannel(Handle)) return ERR::OutOfRange;

#ifdef AUDIO_WORKER
   if (!glAudioWorker) {
      auto self = (extAudio *)Audio;
      if (self->StopWorker) return ERR::NotInitialised;
      if (self->PendingCount >= self->PendingCommands.size()) return ERR::BufferOverflow;
      self->PendingCommands[self->PendingCount++] = AudioCommand(CMD::STOP_LOOPING, Handle);
      wake_audio(self);
      return ERR::Okay;
   }
#endif

   AudioLog log(__FUNCTION__);

   log.traceBranch("Audio: #%d, Channel: $%.8x", Audio->UID, Handle);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);
   if (channel->State != CHS::PLAYING) return ERR::Okay;

   auto &sample = ((extAudio *)Audio)->Samples[channel->SampleHandle];

#ifdef AUDIO_WORKER
   if (sample.Stream) {
      ++sample.Generation;
      sample.DeferredStops = 0;
      sample.Loop2Type = LTYPE::NIL;
      sample.Ring.Used = std::min(sample.Ring.Used, size_t(std::max(0, int(sample.StreamLength - sample.PlayPos))));
      sample.SourceOffset = int(sample.PlayPos) + sample.Ring.Used;
      sample.SourceSeek = true;
      sample.Refilling = sample.RefillPending = false;
      sample.EndOfSource = sample.SourceOffset >= sample.StreamLength;
      sample.Prefilled = true;
      if (!sample.EndOfSource) request_stream((extAudio *)Audio, sample);
      return ERR::Okay;
   }
#endif

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
   if (!Audio or !Handle) return ERR::NullArgs;
   std::lock_guard mixer_lock(((extAudio *)Audio)->MixerMutex);
   if (!((extAudio *)Audio)->GetChannel(Handle)) return ERR::OutOfRange;

#ifdef AUDIO_WORKER
   if (!glAudioWorker) {
      auto self = (extAudio *)Audio;
      if (self->StopWorker) return ERR::NotInitialised;
      if (self->PendingCount >= self->PendingCommands.size()) return ERR::BufferOverflow;
      self->PendingCommands[self->PendingCount++] = AudioCommand(CMD::VOLUME, Handle, Volume);
      wake_audio(self);
      return ERR::Okay;
   }
#endif

   AudioLog log(__FUNCTION__);

   log.traceBranch("Audio: #%d, Channel: $%.8x", Audio->UID, Handle);

   if ((!Audio) or (!Handle)) return log.warning(ERR::NullArgs);

   auto channel = ((extAudio *)Audio)->GetChannel(Handle);

   if (Volume > 1.0) channel->Volume = 1.0;
   else if (Volume < 0) channel->Volume = 0;
   else channel->Volume = Volume;

   set_channel_volume((extAudio *)Audio, channel);
   return ERR::Okay;
}

} // namespace
