#ifdef AUDIO_WORKER

static void dispatch_audio_client(extAudio *Self);

//********************************************************************************************************************

static void execute_audio_command(extAudio *Self, const AudioCommand &Command)
{
   auto channel = Self->GetChannel(Command.Handle);
   if (!channel) return;
   const bool buffering = channel->Buffering;
   channel->Buffering = false;
   switch (Command.CommandID) {
      case CMD::CONTINUE: snd::MixContinue(Self, Command.Handle); break;
      case CMD::PAUSE: snd::pause_channel(Self, Command.Handle); break;
      case CMD::MUTE: snd::MixMute(Self, Command.Handle, std::get<bool>(Command.Data)); break;
      case CMD::PLAY: snd::MixPlay(Self, Command.Handle, std::get<int>(Command.Data)); break;
      case CMD::FREQUENCY: snd::MixFrequency(Self, Command.Handle, std::get<int>(Command.Data)); break;
      case CMD::PAN: snd::MixPan(Self, Command.Handle, std::get<double>(Command.Data)); break;
      case CMD::RATE: snd::MixRate(Self, Command.Handle, std::get<int>(Command.Data)); break;
      case CMD::SAMPLE: snd::MixSample(Self, Command.Handle, std::get<int>(Command.Data)); break;
      case CMD::VOLUME: snd::MixVolume(Self, Command.Handle, std::get<double>(Command.Data)); break;
      case CMD::STOP: snd::MixStop(Self, Command.Handle); break;
      case CMD::STOP_LOOPING: snd::MixStopLoop(Self, Command.Handle); break;
      default: break;
   }
   channel->Buffering = buffering;
}

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
   int offset;
   bool seek;
   {
      std::lock_guard lock(Self->MixerMutex);
      auto &sample = Self->Samples[Handle];
      if (!sample.Stream or !sample.RefillPending or sample.RetryAt > PreciseTime()) return;
      sample.RefillPending = false;
      generation = sample.Generation;
      offset = sample.SourceOffset;
      seek = sample.SourceSeek;
      const int frame_bytes = 1 << sample_shift(sample.SampleType);
      int bytes = sample.Data.size() - sample.Ring.Used;
      if (bytes <= 0) return;
      bytes = std::min(bytes, int(sample.StreamLength) - offset);
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
      source.SampleLength = SAMPLE(bytes / frame_bytes);
   }

   source.Data.resize(source.SampleLength << sample_shift(source.SampleType));
   int bytes = fill_stream_buffer(Handle, source, seek ? offset : -1);
   source.Callback.unpin();
   bytes = std::clamp(bytes, 0, int(source.Data.size()));
   bytes -= bytes % (1 << sample_shift(source.SampleType));
   {
      std::lock_guard lock(Self->MixerMutex);
      if (size_t(Handle) >= Self->Samples.size()) return;
      auto &sample = Self->Samples[Handle];
      if (sample.Generation != generation or !sample.Stream) return;
      sample.Refilling = false;
      bytes = std::min(bytes, std::max(0, int(sample.StreamLength) - offset));
      bytes -= bytes % (1 << sample_shift(sample.SampleType));
      const size_t write_pos = (sample.Ring.Read + sample.Ring.Used) % sample.Data.size();
      const size_t first = std::min(size_t(bytes), sample.Data.size() - write_pos);
      std::copy_n(source.Data.data(), first, sample.Data.data() + write_pos);
      std::copy_n(source.Data.data() + first, bytes - first, sample.Data.data());
      sample.Ring.publish(bytes, sample.Data.size(), 1 << sample_shift(sample.SampleType));
      sample.SourceOffset += bytes;
      sample.SourceSeek = false;
      sample.BufferedLength = BYTELEN(sample.Ring.Used);
      // Loop boundaries do not end pre-roll: keep filling until the ring is full or the producer gives a short read.
      sample.Prefilled |= sample.Ring.Used >= sample.Data.size() or size_t(bytes) < source.Data.size() or
         (sample.SourceOffset >= sample.StreamLength and sample.Loop2Type IS LTYPE::NIL);
      if (sample.SourceOffset >= sample.StreamLength) {
         if (sample.Loop2Type != LTYPE::NIL) {
            sample.SourceSeek = true;
            sample.SourceOffset = sample.Loop2Start << sample_shift(sample.SampleType);
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
   bool pending = Self->NotificationCount != 0;
   #ifdef _WIN32
   pending |= Self->Reopening;
   #endif
   for (const auto &sample : Self->Samples) pending |= sample.RefillPending or sample.DeferredStops;
   if (pending) return ERR::Okay;
   Self->Timer = nullptr;
   return ERR::Terminate;
}

//********************************************************************************************************************

static void dispatch_audio_client(extAudio *Self)
{
   // Bound each dispatch: an unproductive/live producer must not spin the client event loop.
   size_t count;
   {
      std::lock_guard lock(Self->MixerMutex);
      count = Self->Samples.size();
   }
   for (size_t i = 1; i < count; ++i) refill_audio_stream(Self, i);

   for (int delivered = 0; delivered < 256; ++delivered) {
      int handle = 0;
      {
         std::lock_guard lock(Self->MixerMutex);
         const auto now = PreciseTime();
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
      if (handle) audio_stopped_event(*Self, handle);
   }
   if (int error = Self->WorkerError.exchange(0)) {
      kt::Log log("Audio");
      log.warning("Playback failed: %d", error);
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
      pending = Self->NotificationCount != 0;
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
   return false;
}

#endif
