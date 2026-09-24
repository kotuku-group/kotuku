#ifdef ALSA_ENABLED

// The Core thread owns callbacks and object lifetimes.  Only this worker touches the active PCM data path.
// MixerMutex protects channel/sample metadata; no ALSA call or client callback runs with that lock held.

static void execute_audio_command(extAudio *Self, const AudioCommand &Command)
{
   auto channel = Self->GetChannel(Command.Handle);
   if (!channel) return;
   const bool buffering = channel->Buffering;
   channel->Buffering = false;
   switch (Command.CommandID) {
      case CMD::CONTINUE: snd::MixContinue(Self, Command.Handle); break;
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

static void request_stream(extAudio *Self, AudioSample &Sample)
{
   if (!Sample.RefillPending and !Sample.Refilling and !Sample.EndOfSource) {
      Sample.RefillPending = true;
      notify_audio(Self);
   }
}

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

static void dispatch_audio_client(extAudio *Self);

static ERR audio_notification_timer(extAudio *Self, int64_t, int64_t)
{
   dispatch_audio_client(Self);
   std::lock_guard lock(Self->MixerMutex);
   bool pending = Self->NotificationCount != 0;
   for (const auto &sample : Self->Samples) pending |= sample.RefillPending or sample.DeferredStops;
   if (pending) return ERR::Okay;
   Self->Timer = nullptr;
   return ERR::Terminate;
}

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
            if (size_t(event.Sample) < Self->Samples.size() and
                Self->Samples[event.Sample].Generation IS event.Generation) handle = event.Sample;
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
      kt::Log log("ALSA");
      log.warning("Playback failed: %s", snd_strerror(error));
      Self->deactivate();
      return;
   }
   bool pending;
   {
      std::lock_guard lock(Self->MixerMutex);
      pending = Self->NotificationCount != 0;
      for (const auto &sample : Self->Samples) pending |= sample.RefillPending or sample.DeferredStops;
   }
   if (pending and !Self->Timer) {
      kt::SwitchContext context(Self);
      SubscribeTimer(0.01, C_FUNCTION(audio_notification_timer), &Self->Timer);
   }
}

static void audio_client_ready(HOSTHANDLE FD, APTR Data)
{
   uint64_t value;
   (void)read(FD, &value, sizeof(value));
   kt::ScopedObjectLock<extAudio> audio{OBJECTID(uintptr_t(Data))};
   if (audio.granted()) dispatch_audio_client(*audio);
}

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

// All methods below execute on the worker.  Diagnostics are read only after join.
struct AlsaPlayback {
   extAudio *Self;

   bool stopping() const { return Self->StopWorker; }
   uint64_t period_frames() const { return Self->PeriodFrames; }
   uint64_t buffer_frames() const { return Self->BufferFrames; }
   unsigned rate() const { return Self->OutputRate; }
   int64_t available() { return snd_pcm_avail_update(Self->Handle); }
   int64_t write(size_t Offset, size_t Frames) {
      return snd_pcm_writei(Self->Handle, Self->AudioBuffer.data() + Offset * Self->FrameBytes, Frames);
   }
   bool running() { return snd_pcm_state(Self->Handle) IS SND_PCM_STATE_RUNNING; }
   int start() { return snd_pcm_start(Self->Handle); }
   int prepare() { return snd_pcm_prepare(Self->Handle); }
   int resume() { return snd_pcm_resume(Self->Handle); }
   int recover(int Error) { return snd_pcm_recover(Self->Handle, Error, 1); }
   void drop() { snd_pcm_drop(Self->Handle); }
   int64_t delay() {
      snd_pcm_sframes_t frames = 0;
      const int result = snd_pcm_delay(Self->Handle, &frames);
      return result < 0 ? result : frames;
   }
   void fail(int Error) {
      Self->WorkerError = Error;
      notify_audio(Self);
   }
   bool active(bool ApplyCommands) {
      std::lock_guard lock(Self->MixerMutex);
      if (ApplyCommands) {
         for (size_t i = 0; i < Self->PendingCount; ++i) execute_audio_command(Self, Self->PendingCommands[i]);
         Self->PendingCount = 0;
      }
      return audio_playing(Self);
   }
   bool produce(bool Running) {
      std::lock_guard lock(Self->MixerMutex);
      for (size_t i = 0; i < Self->PendingCount; ++i) execute_audio_command(Self, Self->PendingCommands[i]);
      Self->PendingCount = 0;
      if (!audio_playing(Self) and Running) return false;
      int left = Self->PeriodFrames;
      auto dest = Self->AudioBuffer.data();
      while (left > 0) {
         SAMPLE mix_left;
         get_mix_amount(Self, &mix_left);
         const int elements = std::min(left, int(mix_left));
         mix_data(Self, elements, dest);
         process_commands(Self, SAMPLE(elements));
         left -= elements;
         dest += elements * Self->FrameBytes;
      }
      return true;
   }
   int wait(bool Device, int Timeout) {
      auto descriptors = Device ? Self->PollDescriptors.data() : &Self->PollDescriptors.back();
      const auto count = Device ? Self->PollDescriptors.size() : 1;
      const int result = poll(descriptors, count, Timeout);
      if (result < 0) return errno IS EINTR ? 0 : -errno;
      uint64_t value;
      (void)read(Self->WakeFD, &value, sizeof(value));
      if (Device and result > 0) {
         unsigned short events = 0;
         if (snd_pcm_poll_descriptors_revents(Self->Handle, descriptors, count - 1, &events) < 0) return -EIO;
         if (events & (POLLHUP | POLLNVAL)) return -ENODEV;
      }
      return 0;
   }
};

static void audio_worker(extAudio *Self)
{
   glAudioWorker = true;
   AlsaPlayback pcm{Self};
   Self->WorkerStats = run_audio_worker(pcm);
   Self->StopWorker = true;
   glAudioWorker = false;
}

static void stop_audio_worker(extAudio *Self)
{
   {
      std::lock_guard lock(Self->MixerMutex);
      Self->StopWorker = true;
      wake_audio(Self);
   }
   // Do not hold MixerMutex or acquire the Core object lock on the worker during join.
   if (Self->WorkerStarted) {
      pthread_join(Self->Worker, nullptr);
      Self->WorkerStarted = false;
      kt::Log log("ALSA");
      log.msg(VLF::INFO, "Playback diagnostics: %llu underruns, %llu source starvations, %llu recovery failures.",
         (unsigned long long)Self->WorkerStats.Underruns, (unsigned long long)Self->Starvations,
         (unsigned long long)Self->WorkerStats.RecoveryFailures);
      const auto &stats = Self->WorkerStats;
      if (stats.DelaySamples) {
         log.msg(VLF::INFO, "Observed ALSA queue: %llu samples, mean %.2f ms, maximum %.2f ms; negotiated %.2f ms.",
            (unsigned long long)stats.DelaySamples,
            1000.0 * double(stats.DelayTotal) / stats.DelaySamples / Self->OutputRate,
            1000.0 * audio_latency(stats.DelayMaximum, Self->OutputRate),
            1000.0 * audio_latency(Self->BufferFrames, Self->OutputRate));
      }
   }
   if (Self->NotifyFD >= 0) {
      RegisterFD(Self->NotifyFD, RFD::REMOVE|RFD::READ, nullptr, nullptr);
      close(Self->NotifyFD);
      Self->NotifyFD = -1;
   }
   if (Self->WakeFD >= 0) { close(Self->WakeFD); Self->WakeFD = -1; }
   if (Self->Timer) { UpdateTimer(Self->Timer, 0); Self->Timer = nullptr; }
   std::lock_guard lock(Self->MixerMutex);
   Self->PendingCount = Self->NotificationCount = 0;
   for (auto &sample : Self->Samples) {
      ++sample.Generation;
      sample.DeferredStops = 0;
      sample.RefillPending = sample.Refilling = false;
   }
   for (auto &set : Self->Sets) {
      set.Commands.clear();
      for (auto &channel : set.Channel) channel.State = CHS::STOPPED;
      for (auto &channel : set.Shadow) channel.State = CHS::STOPPED;
   }
   Self->reset_lag();
}

static ERR start_audio_worker(extAudio *Self)
{
   const int count = snd_pcm_poll_descriptors_count(Self->Handle);
   if (count <= 0) return ERR::NoSupport;
   Self->PollDescriptors.resize(count + 1);
   if (snd_pcm_poll_descriptors(Self->Handle, Self->PollDescriptors.data(), count) < 0) return ERR::SystemCall;
   Self->WakeFD = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
   Self->NotifyFD = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
   if (Self->WakeFD < 0 or Self->NotifyFD < 0) {
      stop_audio_worker(Self);
      return ERR::SystemCall;
   }
   Self->PollDescriptors.back() = { Self->WakeFD, POLLIN, 0 };
   if (RegisterFD(Self->NotifyFD, RFD::READ, audio_client_ready, (APTR)uintptr_t(Self->UID)) != ERR::Okay) {
      stop_audio_worker(Self);
      return ERR::SystemCall;
   }
   std::fill_n(Self->FilterHistory, 4, 0);
   Self->WorkerError = 0;
   Self->StopWorker = false;
   Self->WorkerStats = {};
   Self->Starvations = 0;
   const int error = pthread_create(&Self->Worker, nullptr, [](void *Data) -> void * {
      pthread_setname_np(pthread_self(), "KotukuAudio");
      audio_worker((extAudio *)Data);
      return nullptr;
   }, Self);
   if (error) {
      stop_audio_worker(Self);
      return ERR::CreateResource;
   }
   Self->WorkerStarted = true;
   return ERR::Okay;
}
#endif
