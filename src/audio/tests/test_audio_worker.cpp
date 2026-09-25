// Included by audio.cpp to exercise the module implementation.

namespace audio_tests_audio_worker {

// This adapter drives the same worker loop as the ALSA module, with no hardware or wall-clock playback.
struct TestPCM {
   AudioTestContext &Test;
   explicit TestPCM(AudioTestContext &Context) : Test(Context) { }
   std::atomic<bool> Stop{false};
   std::atomic<bool> Playing{true};
   std::atomic<bool> Waiting{false};
   bool Blocking = false;
   int WakeFD = -1;
   int Error = 0;
   int RecoveryResult = 0;
   int ResumeResult = 0;
   int PrepareResult = 0;
   int StartResult = 0;
   int WaitResult = 0;
   int WaitLimit = 100;
   int Waits = 0;
   int Produces = 0;
   int Starts = 0;
   int Drops = 0;
   int Resumes = 0;
   int Recoveries = 0;
   int Prepared = 0;
   bool Running = false;
   size_t StopAfter = 512;
   size_t FinishAfter = SIZE_MAX;
   std::deque<int64_t> Writes;
   std::deque<int64_t> Available;
   std::deque<int64_t> Delays;
   std::vector<size_t> Accepted;
   std::array<size_t, 256> Buffer{};
   std::vector<int> Commands;
   std::vector<int> Applied;
   bool InjectCommands = false;
   AudioWorkerStats Stats;

   bool stopping() const { return Stop; }
   unsigned rate() const { return 48000; }
   uint64_t period_frames() const { return 256; }
   uint64_t buffer_frames() const { return 768; }
   static int64_t next(std::deque<int64_t> &Values, int64_t Default) {
      if (Values.empty()) return Default;
      const auto value = Values.front();
      Values.pop_front();
      return value;
   }
   bool active(bool Apply) {
      if (Apply) apply();
      return Playing;
   }
   void apply() {
      Applied.insert(Applied.end(), Commands.begin(), Commands.end());
      Commands.clear();
   }
   bool produce(bool IsRunning) {
      apply();
      if (!Playing and IsRunning) return false;
      for (size_t i = 0; i < Buffer.size(); ++i) Buffer[i] = Produces * Buffer.size() + i;
      ++Produces;
      return true;
   }
   int64_t available() { return next(Available, 768); }
   int64_t write(size_t Offset, size_t Frames) {
      if (Offset and !Commands.empty()) AUDIO_CHECK(Applied.empty());
      const auto result = next(Writes, Frames);
      if (result > 0 and size_t(result) <= Frames) {
         Accepted.insert(Accepted.end(), Buffer.begin() + Offset, Buffer.begin() + Offset + result);
         if (Accepted.size() >= StopAfter) Stop = true;
         if (Accepted.size() >= FinishAfter) Playing = false;
      }
      return result;
   }
   bool running() const { return Running; }
   int start() { ++Starts; Running = StartResult >= 0; return StartResult; }
   int prepare() { ++Prepared; Running = false; return PrepareResult; }
   int resume() { ++Resumes; return ResumeResult; }
   int recover(int ErrorCode) {
      ++Recoveries;
      if (ErrorCode != -EPIPE) return ErrorCode;
      Running = false;
      return RecoveryResult;
   }
   void drop() { ++Drops; Running = false; }
   int64_t delay() { return next(Delays, 0); }
   void fail(int ErrorCode) { Error = ErrorCode; }
   int wait(bool Device, int Timeout) {
      ++Waits;
      if (InjectCommands) {
         // This happens while a partial period remains.  Neither command may execute until its suffix is accepted.
         AUDIO_CHECK(Applied.empty());
         Commands = {17, 23};
         InjectCommands = false;
      }
      if (Blocking) {
         pollfd fd{WakeFD, POLLIN, 0};
         Waiting = true;
         const int result = poll(&fd, 1, Timeout < 0 ? 1000 : Timeout);
         if (result > 0) {
            uint64_t value;
            (void)read(WakeFD, &value, sizeof(value));
         }
         else if (!Device and Timeout < 0) AUDIO_CHECK(false and "Idle stop must wake the worker");
      }
      if (Waits >= WaitLimit) Stop = true;
      return WaitResult;
   }
};

static void retained_writes(AudioTestContext &Test)
{
   TestPCM pcm(Test);
   pcm.Writes = {37, -EAGAIN, 0, -EINTR, -EPIPE, 219, 256};
   pcm.InjectCommands = true;
   const auto stats = run_audio_worker(pcm);
   AUDIO_CHECK(pcm.Error IS 0 and pcm.Produces IS 2);
   AUDIO_CHECK(pcm.Applied IS std::vector<int>({17, 23}));
   AUDIO_CHECK(pcm.Accepted.size() IS 512);
   for (size_t i = 0; i < pcm.Accepted.size(); ++i) AUDIO_CHECK(pcm.Accepted[i] IS i);
   AUDIO_CHECK(stats.Underruns IS 1 and stats.RecoveryFailures IS 0);
   // Recovery needs fresh priming; the retained suffix is only 219 frames.
   AUDIO_CHECK(pcm.Recoveries IS 1 and pcm.Starts IS 0);
}

static void recovery_paths(AudioTestContext &Test)
{
   for (int resumed : {0, -EAGAIN, -EINTR, -ENOSYS}) {
      TestPCM pcm(Test);
      pcm.Writes = {37, -ESTRPIPE, 219, 256};
      pcm.ResumeResult = resumed;
      const auto stats = run_audio_worker(pcm);
      AUDIO_CHECK(pcm.Error IS 0 and pcm.Resumes IS 1);
      AUDIO_CHECK(pcm.Recoveries IS (resumed IS -ENOSYS ? 1 : 0));
      AUDIO_CHECK(stats.RecoveryFailures IS 0 and pcm.Produces IS 2);
      for (size_t i = 0; i < pcm.Accepted.size(); ++i) AUDIO_CHECK(pcm.Accepted[i] IS i);
   }
   for (int error : {-EPIPE, -ENODEV, -EBADFD}) {
      TestPCM pcm(Test);
      pcm.Writes = {37, error};
      pcm.RecoveryResult = -ENODEV;
      const auto stats = run_audio_worker(pcm);
      AUDIO_CHECK(pcm.Error < 0 and stats.RecoveryFailures IS 1);
      AUDIO_CHECK(pcm.Accepted.size() IS 37 and pcm.Produces IS 1 and pcm.Drops IS 1);
   }
   TestPCM suspended(Test);
   suspended.Available = {-ESTRPIPE, -ESTRPIPE, -ESTRPIPE};
   suspended.ResumeResult = -EAGAIN;
   suspended.WaitLimit = 3;
   run_audio_worker(suspended);
   AUDIO_CHECK(suspended.Resumes IS 3 and suspended.Produces IS 0 and suspended.Error IS 0);
}

static void state_transitions(AudioTestContext &Test)
{
   TestPCM full(Test);
   run_audio_worker(full);
   AUDIO_CHECK(full.Starts IS 1 and full.Produces IS 2 and full.Accepted.size() IS 512);

   TestPCM unavailable(Test);
   unavailable.Available = {-EAGAIN, -EINTR, -EPIPE};
   const auto recovered = run_audio_worker(unavailable);
   AUDIO_CHECK(recovered.Underruns IS 1 and unavailable.Produces IS 2 and unavailable.Starts IS 1);

   TestPCM start_failure(Test);
   start_failure.StartResult = -ENODEV;
   run_audio_worker(start_failure);
   AUDIO_CHECK(start_failure.Error IS -ENODEV);

   TestPCM invalid_write(Test);
   invalid_write.Writes = {257};
   run_audio_worker(invalid_write);
   AUDIO_CHECK(invalid_write.Error IS -EIO and invalid_write.Produces IS 1);

   TestPCM idle(Test);
   idle.Playing = false;
   idle.WaitLimit = 2;
   run_audio_worker(idle);
   AUDIO_CHECK(idle.Produces IS 0 and idle.Prepared IS 2 and idle.Starts IS 0);

   TestPCM drain(Test);
   drain.StopAfter = SIZE_MAX;
   drain.FinishAfter = 512;
   drain.Delays = {512, 256, 0};
   drain.WaitLimit = 3;
   const auto drained = run_audio_worker(drain);
   AUDIO_CHECK(drain.Produces IS 2 and drain.Starts IS 1 and drain.Prepared IS 1);
   AUDIO_CHECK(drained.DelaySamples IS 3 and drained.DelayTotal IS 768);
   AUDIO_CHECK(drain.Accepted.size() IS 512); // Idle waits must not replenish silence.

   TestPCM prepare_failure(Test);
   prepare_failure.Playing = false;
   prepare_failure.PrepareResult = -ENODEV;
   run_audio_worker(prepare_failure);
   AUDIO_CHECK(prepare_failure.Error IS -ENODEV and prepare_failure.Waits IS 0);

   TestPCM poll_failure(Test);
   poll_failure.StopAfter = 4096;
   poll_failure.WaitResult = -ENODEV;
   poll_failure.Delays = {512};
   const auto observed = run_audio_worker(poll_failure);
   AUDIO_CHECK(poll_failure.Error IS -ENODEV);
   AUDIO_CHECK(observed.DelaySamples IS 1 and observed.DelayMaximum IS 512 and observed.DelayTotal IS 512);
}

static void stop_and_join(AudioTestContext &Test, bool Active, bool Recovering)
{
   TestPCM pcm(Test);
   pcm.Blocking = true;
   pcm.Playing = Active;
   pcm.StopAfter = SIZE_MAX;
   pcm.WakeFD = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
   AUDIO_REQUIRE(pcm.WakeFD >= 0);
   if (Recovering) {
      pcm.Available = {-ESTRPIPE};
      pcm.ResumeResult = -EAGAIN;
   }
   pthread_t worker;
   const int created = pthread_create(&worker, nullptr, [](void *Data) -> void * {
      auto &pcm = *(TestPCM *)Data;
      pcm.Stats = run_audio_worker(pcm);
      return nullptr;
   }, &pcm);
   if (!Test.check(created IS 0, "pthread_create", __FILE__, __LINE__)) {
      close(pcm.WakeFD);
      return;
   }
   // Bounded handshake; the worker waits on the same eventfd used to wake it for shutdown.
   for (int i = 0; i < 1000 and !pcm.Waiting; ++i) usleep(1000);
   AUDIO_CHECK(pcm.Waiting);
   pcm.Stop = true;
   const uint64_t value = 1;
   AUDIO_CHECK(write(pcm.WakeFD, &value, sizeof(value)) IS sizeof(value));
   AUDIO_CHECK(pthread_join(worker, nullptr) IS 0);
   close(pcm.WakeFD);
   AUDIO_CHECK(pcm.Error IS 0 and pcm.Drops > 0);
   if (!Active) AUDIO_CHECK(pcm.Produces IS 0);
   if (Recovering) AUDIO_CHECK(pcm.Resumes > 0);
}

static void command_wakes_idle_worker(AudioTestContext &Test)
{
   TestPCM pcm(Test);
   pcm.Blocking = true;
   pcm.Playing = false;
   pcm.WakeFD = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
   AUDIO_REQUIRE(pcm.WakeFD >= 0);
   pthread_t worker;
   const int created = pthread_create(&worker, nullptr, [](void *Data) -> void * {
      auto &pcm = *(TestPCM *)Data;
      pcm.Stats = run_audio_worker(pcm);
      return nullptr;
   }, &pcm);
   if (!Test.check(created IS 0, "pthread_create", __FILE__, __LINE__)) {
      close(pcm.WakeFD);
      return;
   }
   for (int i = 0; i < 1000 and !pcm.Waiting; ++i) usleep(1000);
   AUDIO_CHECK(pcm.Waiting);
   pcm.Playing = true;
   const uint64_t value = 1;
   AUDIO_CHECK(write(pcm.WakeFD, &value, sizeof(value)) IS sizeof(value));
   AUDIO_CHECK(pthread_join(worker, nullptr) IS 0);
   close(pcm.WakeFD);
   AUDIO_CHECK(pcm.Produces IS 2 and pcm.Accepted.size() IS 512 and pcm.Error IS 0);
}

static void run(AudioTestContext &Test)
{
   retained_writes(Test);
   recovery_paths(Test);
   state_transitions(Test);
   command_wakes_idle_worker(Test);
   for (int i = 0; i < 8; ++i) {
      stop_and_join(Test, false, false);
      stop_and_join(Test, true, false);
      stop_and_join(Test, true, true);
   }
}

} // namespace audio_tests_audio_worker
