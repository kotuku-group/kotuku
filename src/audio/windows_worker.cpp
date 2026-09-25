#ifdef _WIN32

static bool windows_render(void *Context, float *Output, unsigned Frames, unsigned Padding)
{
   auto self = (extAudio *)Context;
   std::unique_lock lock(self->MixerMutex, std::try_to_lock);
   if (!lock.owns_lock()) {
      ++self->MixerContentions; // Worker-owned; inspected only after joining.
      std::fill_n(Output, size_t(Frames) * (self->Stereo ? 2 : 1), 0.0f);
      return true;
   }
   if (self->StopWorker) return false;
   glAudioWorker = true;
   self->QueuedFrames = Padding + Frames;
   self->QueuedAt = PreciseTime();
   flush_audio_commands(self);
   bool active = audio_playing(self);
   if (active) {
      bool effects = !self->GlobalEffects->Effects.empty();
      for (const auto &set : self->Sets) effects |= set.Effects and !set.Effects->Effects.empty();
      self->TailFrames = effects ? self->OutputRate * 2 : 0;
   }
   else if (self->TailFrames) {
      self->TailFrames -= std::min(self->TailFrames, Frames);
      active = true;
   }
   if (active) {
      unsigned left = Frames;
      while (left) {
         SAMPLE boundary;
         get_mix_amount(self, &boundary);
         const unsigned count = std::min(left, unsigned(boundary));
         mix_data(self, count, Output);
         process_commands(self, SAMPLE(count));
         left -= count;
         Output += count * (self->Stereo ? 2 : 1);
      }
   }
   else {
      self->QueuedFrames = Padding;
      std::fill_n(self->FilterHistory, 4, 0.0f);
      auto reset = [](AudioEffectChain &Chain) {
         for (auto effect : Chain.Effects) effect->ResetPending = true;
      };
      reset(*self->GlobalEffects);
      for (auto &set : self->Sets) {
         if (set.Effects) reset(*set.Effects);
      }
   }
   glAudioWorker = false;
   return active;
}

static void windows_failed(void *Context, int Error)
{
   auto self = (extAudio *)Context;
   self->WorkerError = Error;
   notify_audio(self);
}

static void windows_client_ready(HOSTHANDLE, APTR Data)
{
   kt::ScopedObjectLock<extAudio> audio{OBJECTID(uintptr_t(Data))};
   if (audio.granted()) dispatch_audio_client(*audio);
}

static ERR init_audio(extAudio *Self)
{
   WasapiFormat format;
   format.Rate = Self->OutputRate;
   format.Channels = (Self->Flags & ADF::STEREO) != ADF::NIL ? 2 : 1;
   format.Period = Self->PeriodSize;
   Self->RenderStream = wasapi_open(Self->Device.c_str(), format, Self, windows_render, windows_failed);
   if (!Self->RenderStream) return ERR::NoSupport;
   Self->OutputRate = format.Rate;
   Self->Stereo = format.Channels IS 2;
   Self->BitDepth = 32;
   Self->PeriodFrames = Self->PeriodSize = format.Period;
   Self->BufferFrames = format.Capacity;
   Self->Periods = (format.Capacity + format.Period - 1) / format.Period;
   Self->FrameBytes = format.Channels * sizeof(float);
   Self->MasterVolume = Self->Volumes[0].Channels[0];
   Self->Mute = (Self->Volumes[0].Flags & VCF::MUTE) != VCF::NIL;
   if (format.Endpoint[0]) kt::Log("WASAPI").msg(VLF::INFO, "%s: %u Hz, %u-frame period, %u-frame capacity.",
      format.Endpoint, format.Rate, format.Period, format.Capacity);
   return ERR::Okay;
}

static ERR start_audio_worker(extAudio *Self)
{
   if (RegisterFD(wasapi_notification(Self->RenderStream), RFD::READ, windows_client_ready,
         (APTR)uintptr_t(Self->UID)) != ERR::Okay) return ERR::SystemCall;
   std::fill_n(Self->FilterHistory, 4, 0.0f);
   Self->WorkerError = 0;
   Self->StopWorker = false;
   Self->WorkerStarted = true;
   Self->WorkerStartedAt = PreciseTime();
   if (!Self->Reopening) Self->ReopenAttempts = 0;
   Self->MixerContentions = 0;
   Self->Starvations = 0;
   wasapi_start(Self->RenderStream);
   return ERR::Okay;
}

static void stop_audio_worker(extAudio *Self)
{
   Self->StopWorker = true;
   // Join outside the mixer lock; the transport owns all COM teardown.
   if (Self->RenderStream) {
      RegisterFD(wasapi_notification(Self->RenderStream), RFD::REMOVE | RFD::READ, nullptr, nullptr);
      WasapiDiagnostics stats;
      wasapi_close(Self->RenderStream, &stats);
      Self->RenderStream = nullptr;
      if (stats.Packets) {
         auto percentile = [&](unsigned Percent) {
            uint64_t count = 0;
            for (unsigned i = 0; i < 64; ++i) {
               count += stats.Histogram[i];
               if (count * 100 >= stats.Packets * Percent) return (i + 1) * 100;
            }
            return 6400u;
         };
         kt::Log("WASAPI").msg(VLF::INFO,
            "Render: %llu packets, %llu wakes, %llu empty queues, %llu starvations, %llu mixer contentions; "
            "microseconds mean %.1f, p50 <%u, p95 <%u, p99 <%u, max %llu; mean queue %.1f frames.",
            stats.Packets, stats.Wakeups, stats.EmptyQueues, Self->Starvations, Self->MixerContentions,
            double(stats.RenderMicroseconds) / stats.Packets, percentile(50), percentile(95), percentile(99),
            stats.MaximumMicroseconds, double(stats.QueuedTotal) / stats.Packets);
         kt::Log("WASAPI").msg(VLF::INFO, "Process CPU %.3f seconds over %.3f seconds (%.2f%% of one core).",
            double(stats.ProcessMicroseconds) / 1000000, double(stats.ElapsedMicroseconds) / 1000000,
            stats.ElapsedMicroseconds ? 100.0 * stats.ProcessMicroseconds / stats.ElapsedMicroseconds : 0);
      }
   }
   if (Self->Timer) { UpdateTimer(Self->Timer, 0); Self->Timer = nullptr; }
   std::lock_guard lock(Self->MixerMutex);
   Self->WorkerStarted = false;
   Self->Reopening = false;
   cancel_audio_batches(Self);
   Self->PendingCount = Self->NotificationCount = 0;
   Self->QueuedFrames = Self->TailFrames = 0;
   for (auto &sample : Self->Samples) {
      ++sample.Generation;
      sample.DeferredStops = 0;
      sample.RefillPending = sample.Refilling = false;
   }
   for (auto &set : Self->Sets) {
      set.Commands.clear();
      for (auto &channel : set.Channel) {
         channel.State = CHS::STOPPED;
         channel.Paused = false;
      }
      for (auto &channel : set.Shadow) channel.State = CHS::STOPPED;
   }
}

static void reopen_windows_audio(extAudio *Self)
{
   if (PreciseTime() < Self->ReopenAt) return;
   // Preserve source cursors, rings and queued commands while replacing the endpoint.
   Self->StopWorker = true;
   if (Self->RenderStream) {
      RegisterFD(wasapi_notification(Self->RenderStream), RFD::REMOVE | RFD::READ, nullptr, nullptr);
      wasapi_close(Self->RenderStream);
      Self->RenderStream = nullptr;
   }
   Self->WorkerStarted = false;
   Self->QueuedFrames = 0;
   if (Self->ReopenAttempts >= 20) {
      kt::Log("Audio").warning("The audio endpoint could not be reopened after 20 attempts.");
      stop_audio_worker(Self);
      return;
   }
   ++Self->ReopenAttempts;
   if (init_audio(Self) IS ERR::Okay) {
      std::lock_guard lock(Self->MixerMutex);
      Self->DriverBitSize = Self->FrameBytes;
      Self->MixElements = SAMPLE(Self->BufferFrames);
      Self->MixBuffer.resize(Self->BufferFrames * (Self->Stereo ? 2 : 1));
      Self->MixConfig = AudioConfig(Self->Stereo, (Self->Flags & ADF::OVER_SAMPLING) != ADF::NIL);
      configure_effects(*Self->GlobalEffects, Self->OutputRate, Self->Stereo);
      for (auto &set : Self->Sets) {
         set.ScratchBuffer.resize(Self->MixBuffer.size());
         set.MixLeft = set.TickFrames(Self->OutputRate);
         if (set.Effects) configure_effects(*set.Effects, Self->OutputRate, Self->Stereo);
      }
      if (start_audio_worker(Self) IS ERR::Okay) {
         Self->Reopening = false;
         return;
      }
   }
   Self->ReopenAt = PreciseTime() + 250000;
}
#endif
