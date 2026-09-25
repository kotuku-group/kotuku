#ifdef ALSA_ENABLED

// The Core thread owns callbacks and object lifetimes.  Only this worker touches the active PCM data path.
// MixerMutex protects channel/sample metadata; no ALSA call or client callback runs with that lock held.

static void audio_client_ready(HOSTHANDLE FD, APTR Data)
{
   uint64_t value;
   (void)read(FD, &value, sizeof(value));
   kt::ScopedObjectLock<extAudio> audio{OBJECTID(uintptr_t(Data))};
   if (audio.granted()) dispatch_audio_client(*audio);
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
   cancel_audio_batches(Self);
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
