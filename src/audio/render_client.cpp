
static ERR apply_audio_command(extAudio *Self, const AudioCommand &Command)
{
   auto channel = Self->GetChannel(Command.Handle);
   if (!channel) return ERR::OutOfRange;
   if (Command.DeferredError != ERR::Okay) return Command.DeferredError;
   switch (Command.CommandID) {
      case CMD::CONTINUE:  return snd::MixContinue(Self, Command.Handle);
      case CMD::PAUSE:     return snd::MixPause(Self, Command.Handle);
      case CMD::MUTE:      return snd::MixMute(Self, Command.Handle, std::get<bool>(Command.Data));
      case CMD::PLAY:      return snd::MixPlay(Self, Command.Handle, std::get<int64_t>(Command.Data));
      case CMD::FREQUENCY: return snd::MixFrequency(Self, Command.Handle, std::get<int>(Command.Data));
      case CMD::PAN:       return snd::MixPan(Self, Command.Handle, std::get<double>(Command.Data));
      case CMD::TEMPO:     return snd::MixTempo(Self, Command.Handle, std::get<int>(Command.Data));
      case CMD::SAMPLE:    return snd::MixSample(Self, Command.Handle, std::get<int>(Command.Data));
      case CMD::VOLUME:    return snd::MixVolume(Self, Command.Handle, std::get<double>(Command.Data));
      case CMD::STOP:      return snd::MixStop(Self, Command.Handle);
      case CMD::RELEASE:   return snd::MixRelease(Self, Command.Handle);
      default:             return ERR::Args;
   }
}

//********************************************************************************************************************

static void execute_audio_command(extAudio *Self, const AudioCommand &Command)
{
   const auto error = apply_audio_command(Self, Command);
   Self->BatchCompletions.complete(Command.CompletionSlot, Command.CommandIndex, error);
}

//********************************************************************************************************************
// Cancellation completes every command before discarding queues, independently of channel lifetime.

static void cancel_audio_batches(extAudio *Self, unsigned SetIndex)
{
   for (size_t i = 1; i < Self->Sets.size(); ++i) {
      if (SetIndex and i != SetIndex) continue;
      for (const auto &command : Self->Sets[i].Commands) {
         Self->BatchCompletions.complete(command.CompletionSlot, command.CommandIndex, ERR::Cancelled);
      }
      Self->Sets[i].Commands.clear();
   }
}

//********************************************************************************************************************
// This client timer survives device shutdown.  Core holds the Audio object lock throughout dispatch.

static ERR audio_batch_timer(extAudio *Self, int64_t, int64_t)
{
   size_t count;

   {
      std::lock_guard lock(Self->MixerMutex);
      if (Self->DispatchingBatches) return ERR::Okay;
      Self->DispatchingBatches = true;
      count = std::min(size_t(128), Self->BatchCompletions.available());
   }

   for (size_t i = 0; i < count; ++i) {
      AudioCompletions::Entry result;
      {
         std::lock_guard lock(Self->MixerMutex);
         if (!Self->BatchCompletions.take(result)) break;
      }

      auto &callback = result.Callback;
      if (!callback.stale()) {
         if (callback.isC()) {
            kt::SwitchContext context(callback.Context);
            auto routine = (void (*)(extAudio *, ERR, int, APTR))callback.Routine;
            routine(Self, result.Error, result.FailedCommand, callback.Meta);
         }
         else if (callback.isScript()) {
            sc::Call(callback, std::to_array<ScriptArg>({
               { "Audio", Self, FD_OBJECTPTR }, { "Error", int(result.Error) },
               { "FailedCommand", result.FailedCommand }
            }));
         }
      }

      release_audio_callback(callback);
      if (Self->collecting()) break;
   }

   std::lock_guard lock(Self->MixerMutex);
   Self->DispatchingBatches = false;
   if (Self->BatchCompletions.pending()) return ERR::Okay;
   Self->BatchTimer = nullptr;
   return ERR::Terminate;
}

#ifdef AUDIO_WORKER

static void dispatch_audio_client(extAudio *Self);

//********************************************************************************************************************
// Sound's synchronous channel queries use the same serialisation boundary as a mixer period.
// This also preserves channel reservation semantics for overlapping Sound.Activate calls.

static void flush_audio_commands(extAudio *Self)
{
   const bool previous = glAudioWorker;
   glAudioWorker = true;
   for (size_t i = 0; i < Self->PendingCount; ++i) execute_audio_command(Self, Self->PendingCommands[i]);
   Self->PendingCount = 0;
   glAudioWorker = previous;
}

//********************************************************************************************************************

static void request_stream(extAudio *Self, AudioSample &Sample)
{
   if (!Sample.RefillPending and !Sample.Refilling and !Sample.EndOfSource) {
      Sample.RefillPending = true;
      notify_audio(Self);
   }
}

//********************************************************************************************************************
// Read into a private scratch buffer.  A callback may seek, remove samples or resize the sample table;
// publish its result only if the same generation is still present afterwards.

static void refill_audio_stream(extAudio *Self, int Handle)
{
   AudioSample source;
   uint64_t generation;
   int64_t offset;
   bool seek;

   {
      std::lock_guard lock(Self->MixerMutex);
      auto &sample = Self->Samples[Handle];
      if (!sample.Stream or !sample.RefillPending or sample.RetryAt > PreciseTime()) return;

      sample.RefillPending = false;
      generation = sample.Generation;
      offset     = sample.SourceOffset;
      seek       = sample.SourceSeek;
      const int frame_bytes = sample.FrameBytes;

      int bytes = sample.Data.size() - sample.Ring.Used;
      if (bytes <= 0) return;

      if (sample.StreamLengthKnown) {
         bytes = int(std::clamp<int64_t>(sample.StreamLength - offset, 0, bytes));
      }

      bytes -= bytes % frame_bytes;
      if (bytes <= 0 or sample.Callback.stale()) {
         sample.EndOfSource = true;
         sample.Prefilled = true;
         wake_audio(Self);
         return;
      }

      sample.Refilling = true;
      source.Callback = sample.Callback;
      source.Callback.pin();
      source.SampleType = sample.SampleType;
      source.FrameBytes = sample.FrameBytes;
      source.Swap = sample.Swap;
      source.SampleLength = SAMPLE(bytes / frame_bytes);
   }

   source.Data.resize(size_t(source.SampleLength) * source.FrameBytes);
   int bytes = fill_stream_buffer(Handle, source, seek ? offset : -1);
   source.Callback.unpin();
   bytes = std::clamp(bytes, 0, int(source.Data.size()));
   const bool partial = (bytes % source.FrameBytes) != 0;
   bytes -= bytes % source.FrameBytes;
   if (source.Swap) swap_samples_16(source.Data.data(), bytes);

   {
      std::lock_guard lock(Self->MixerMutex);
      if (size_t(Handle) >= Self->Samples.size()) return;

      auto &sample = Self->Samples[Handle];
      if (sample.Generation != generation or !sample.Stream) return;
      sample.Refilling = false;

      if (sample.StreamLengthKnown) {
         bytes = int(std::min<int64_t>(bytes, std::max<int64_t>(0, sample.StreamLength - offset)));
      }

      bytes -= bytes % sample.FrameBytes;
      const size_t write_pos = (sample.Ring.Read + sample.Ring.Used) % sample.Data.size();
      const size_t first = std::min(size_t(bytes), sample.Data.size() - write_pos);
      std::copy_n(source.Data.data(), first, sample.Data.data() + write_pos);
      std::copy_n(source.Data.data() + first, bytes - first, sample.Data.data());
      sample.Ring.publish(bytes, sample.Data.size(), sample.FrameBytes);
      sample.SourceOffset += bytes;
      sample.SourceSeek = partial; // Discarded partial-frame bytes are requested again from an explicit offset.
      sample.BufferedLength = BYTELEN(sample.Ring.Used);

      // Loop boundaries do not end pre-roll: keep filling until the ring is full or the producer gives a short read.

      sample.Prefilled |= sample.Ring.Used >= sample.Data.size() or size_t(bytes) < source.Data.size() or
         (sample.StreamLengthKnown and sample.SourceOffset >= sample.StreamLength and
         !sample.streamLoops());

      if (sample.StreamLengthKnown and sample.SourceOffset >= sample.StreamLength) {
         if (sample.streamLoops()) {
            sample.SourceSeek = true;
            sample.SourceOffset = int64_t(sample.Loop2Start) * sample.FrameBytes;
            request_stream(Self, sample);
         }
         else sample.EndOfSource = true;
      }

      if (bytes) sample.Starved = false;
      else if (!sample.EndOfSource) {
         sample.RetryAt = PreciseTime() + 10000;
         sample.RefillPending = true;
      }
   }

   wake_audio(Self);
}

//********************************************************************************************************************

static ERR audio_notification_timer(extAudio *Self, int64_t, int64_t)
{
   dispatch_audio_client(Self);
   std::lock_guard lock(Self->MixerMutex);
   bool pending = Self->NotificationCount != 0 or !Self->DrainingNotifications.empty();

#ifdef _WIN32
   pending |= Self->Reopening;
#endif

   for (const auto &sample : Self->Samples) pending |= sample.RefillPending or sample.DeferredStops;
   if (pending) return ERR::Okay;
   Self->Timer = nullptr;
   return ERR::Terminate;
}

//********************************************************************************************************************

static bool voices_active(const ChannelSet &Set)
{
   for (const auto &channel : Set.Channel) if (channel.active() and !channel.isStopped()) return true;
   for (const auto &channel : Set.Shadow) if (channel.active() and !channel.isStopped()) return true;
   return false;
}

//********************************************************************************************************************
// Move tail waits to client-owned storage.  Allocation occurs after releasing the mixer lock, and waiting tails no
// longer occupy the bounded worker notification queue.

static void transfer_draining_stops(extAudio *Self)
{
   std::array<Notification, 256> draining;
   size_t count = 0;

   {
      std::lock_guard lock(Self->MixerMutex);
      size_t retained = 0;
      for (size_t i = 0; i < Self->NotificationCount; ++i) {
         if (Self->Notifications[i].AwaitDrain) draining[count++] = Self->Notifications[i];
         else Self->Notifications[retained++] = Self->Notifications[i];
      }
      Self->NotificationCount = retained;
   }

   for (size_t i = 0; i < count; ++i) Self->DrainingNotifications.push_back(draining[i]);
}

//********************************************************************************************************************
// Drain state of a stopped voice's effect path, being its channel set's chain followed by the global chain.  Other
// audio on the path makes the tail inseparable, so it reports ACTIVE rather than waiting for a drain that may never
// end.  Caller holds the mixer lock.

static ADS stop_path_state(extAudio *Self, size_t SetIndex)
{
   if (SetIndex >= Self->Sets.size()) return ADS::IDLE;
   const auto &set = Self->Sets[SetIndex];
   if (voices_active(set)) return ADS::ACTIVE;
   if (set.Effects and set.Effects->pending()) return ADS::DRAINING;
   if (!Self->GlobalEffects->pending()) return ADS::IDLE;

   for (size_t i = 1; i < Self->Sets.size(); ++i) {
      if (i IS SetIndex) continue;
      const auto &other = Self->Sets[i];
      if (voices_active(other) or (other.Effects and other.Effects->pending())) return ADS::ACTIVE;
   }
   return ADS::DRAINING;
}

//********************************************************************************************************************
// Set the due time of stops that are waiting for their effect path to drain.  A drained path is due after the device
// queue.  If other audio keeps the path active, the voice's own tail cannot be observed, so it is due once the path's
// estimated 60 dB decay has elapsed after the voice was last heard.  Deactivation and the deadline end the wait.
// Caller holds the mixer lock.

static void resolve_drained_stop(extAudio *Self, Notification &Event, int64_t Now)
{
   if (!Event.AwaitDrain) return;

   const auto set = unsigned(Event.Channel) >> 16;
   const auto state = Self->EffectConfigured ? stop_path_state(Self, set) : ADS::IDLE;
   if ((state IS ADS::DRAINING) and (Now < Event.Deadline)) return;

   Event.AwaitDrain = false;
   if (!Self->EffectConfigured) Event.Due = Now;
   else if ((state IS ADS::ACTIVE) and (Self->OutputRate > 0)) {
      double frames = double(Self->GlobalEffects->decay_estimate());
      if ((set < Self->Sets.size()) and Self->Sets[set].Effects) {
         frames += double(Self->Sets[set].Effects->decay_estimate());
      }
      const double decay = std::min(frames * 1000000.0 / Self->OutputRate,
         double(Event.Deadline - Event.Audible));
      Event.Due = std::max(Now, Event.Audible + int64_t(std::max(0.0, decay)));
   }
   else Event.Due = Now + int64_t(Self->MixerLag() * 1000000);
}

static void resolve_drained_stops(extAudio *Self, int64_t Now)
{
   for (size_t i = 0; i < Self->NotificationCount; ++i) {
      resolve_drained_stop(Self, Self->Notifications[i], Now);
   }
}

//********************************************************************************************************************

static void dispatch_audio_client(extAudio *Self)
{
   // Bound each dispatch: an unproductive/live producer must not spin the client event loop.

   transfer_draining_stops(Self);

   size_t count;

   {
      std::lock_guard lock(Self->MixerMutex);
      count = Self->Samples.size();
   }

   for (size_t i = 1; i < count; ++i) refill_audio_stream(Self, i);

   for (int delivered = 0; delivered < 256; ++delivered) {
      int handle = 0;
      size_t remove_draining = SIZE_MAX;

      {
         std::lock_guard lock(Self->MixerMutex);
         const auto now = PreciseTime();
         resolve_drained_stops(Self, now);
         size_t draining = SIZE_MAX;
         const auto draining_count = Self->DrainingNotifications.size();
         size_t inspected = 0;
         for (; inspected < draining_count and inspected < 256; ++inspected) {
            const auto index = (Self->DrainingCursor + inspected) % draining_count;
            resolve_drained_stop(Self, Self->DrainingNotifications[index], now);
            if (Self->DrainingNotifications[index].Due <= now) {
               draining = index;
               break;
            }
         }
         if (draining_count) Self->DrainingCursor = (Self->DrainingCursor + inspected) % draining_count;
         size_t index = 0;
         for (; index < Self->NotificationCount; ++index) {
            if (Self->Notifications[index].Due <= now) break;
         }

         if (index < Self->NotificationCount) {
            auto event = Self->Notifications[index];

            std::move(Self->Notifications.begin() + index + 1,
               Self->Notifications.begin() + Self->NotificationCount, Self->Notifications.begin() + index);
            --Self->NotificationCount;

            auto channel = Self->GetChannel(event.Channel);
            if (size_t(event.Sample) < Self->Samples.size() and channel and
                Self->Samples[event.Sample].Generation IS event.SampleGeneration and
                channel->PlaybackGeneration IS event.PlaybackGeneration) handle = event.Sample;
         }
         else if (draining != SIZE_MAX and
               Self->DrainingNotifications[draining].Due <= now) {
            auto event = Self->DrainingNotifications[draining];
            remove_draining = draining;

            auto channel = Self->GetChannel(event.Channel);
            if (size_t(event.Sample) < Self->Samples.size() and channel and
                Self->Samples[event.Sample].Generation IS event.SampleGeneration and
                channel->PlaybackGeneration IS event.PlaybackGeneration) handle = event.Sample;
         }
         else {
            for (size_t i = 1; i < Self->Samples.size(); ++i) {
               auto &sample = Self->Samples[i];
               if (sample.DeferredStops and sample.DeferredStopDue <= now) {
                  --sample.DeferredStops;
                  handle = i;
                  break;
               }
            }
            if (!handle) break;
         }
      }

      if (remove_draining != SIZE_MAX) {
         Self->DrainingNotifications.erase(Self->DrainingNotifications.begin() + remove_draining);
         if (Self->DrainingNotifications.empty()) Self->DrainingCursor = 0;
         else if (Self->DrainingCursor >= Self->DrainingNotifications.size()) Self->DrainingCursor = 0;
      }
      if (handle) audio_stopped_event(*Self, handle);
   }

   if (int error = Self->WorkerError.exchange(0)) {
      kt::Log().warning("Playback failed: %d", error);
#ifdef _WIN32
      Self->Reopening = true;
      if (PreciseTime() - Self->WorkerStartedAt >= 5000000) Self->ReopenAttempts = 0;
      Self->ReopenAt = PreciseTime() + 250000;
#else
      Self->deactivate();
      return;
#endif
   }

#ifdef _WIN32
   if (Self->Reopening) reopen_windows_audio(Self);
#endif

   bool pending;

   {
      std::lock_guard lock(Self->MixerMutex);
      pending = Self->NotificationCount != 0 or !Self->DrainingNotifications.empty();
#ifdef _WIN32
      pending |= Self->Reopening;
#endif
      for (const auto &sample : Self->Samples) pending |= sample.RefillPending or sample.DeferredStops;
   }

   if (pending and !Self->Timer) {
      kt::SwitchContext context(Self);
      SubscribeTimer(0.01, C_FUNCTION(audio_notification_timer), &Self->Timer);
   }
}

//********************************************************************************************************************

static bool audio_playing(extAudio *Self)
{
   for (const auto &set : Self->Sets) {
      if (!set.Commands.empty()) return true;
      if (set.Effects and effects_pending(*set.Effects)) return true;

      for (const auto &channel : set.Channel) {
         if (channel.Frequency > 0 and channel.State != CHS::STOPPED and channel.State != CHS::FINISHED) {
            const auto &sample = Self->Samples[channel.SampleHandle];
            if (!sample.Stream or sample.Prefilled) return true;
         }
      }

      for (const auto &channel : set.Shadow) {
         if (channel.Frequency > 0 and channel.State != CHS::STOPPED and channel.State != CHS::FINISHED) {
            const auto &sample = Self->Samples[channel.SampleHandle];
            if (!sample.Stream or sample.Prefilled) return true;
         }
      }
   }

   if (effects_pending(*Self->GlobalEffects)) return true;

   // A stop command can remove the last source without another render call. Finalise its meter interval and
   // residual history here, after DSP has drained; retained periods and device frames still drain separately.

   auto idle = [](AudioEffectChain &Chain) {
      Chain.State = ADS::IDLE;
      for (auto effect : Chain.Effects) {
         if (effect->MeterFrames or (effect->Meter.Flags & AMF::IDLE) IS AMF::NIL) effect->idle();
      }
   };

   idle(*Self->GlobalEffects);
   for (auto &set : Self->Sets) if (set.Effects) idle(*set.Effects);
   return false;
}

#endif
