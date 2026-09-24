#pragma once

#include "audio_buffer.h"
#include <cerrno>

// The PCM adapter owns device calls, waits and mixer access.  Keeping the loop independent of ALSA permits
// deterministic failure injection without exposing test controls through the public Audio API.
struct AudioWorkerStats {
   uint64_t Underruns = 0;
   uint64_t RecoveryFailures = 0;
   uint64_t DelaySamples = 0;
   uint64_t DelayTotal = 0;
   uint64_t DelayMaximum = 0;

   void observe(int64_t Frames) {
      if (Frames < 0) return;
      ++DelaySamples;
      DelayTotal += Frames;
      DelayMaximum = std::max(DelayMaximum, uint64_t(Frames));
   }
};

template<class Backend> AudioWorkerStats run_audio_worker(Backend &PCM)
{
   AudioWorkerStats stats;
   RetainedAudioPeriod period;
   uint64_t primed = 0;
   bool running = false;
   bool failed = false;
   auto fail = [&](int Error) {
      failed = true;
      PCM.fail(Error);
   };
   auto recover = [&](int Error) {
      if (Error IS -EAGAIN or Error IS -EINTR) return true;
      if (Error IS -EPIPE) ++stats.Underruns;
      if (Error IS -ESTRPIPE) {
         const int resumed = PCM.resume();
         if (resumed IS -EAGAIN or resumed IS -EINTR or resumed >= 0) return true;
         Error = -EPIPE;
      }
      const int result = PCM.recover(Error);
      if (result < 0) {
         ++stats.RecoveryFailures;
         fail(result);
         return false;
      }
      primed = 0;
      running = false;
      return true;
   };

   while (!PCM.stopping() and !failed) {
      const bool active = PCM.active(!period.Remaining);
      if (!active and !period.Remaining and (!primed or running)) {
         const auto delay = running ? PCM.delay() : 0;
         if (running and delay >= 0) stats.observe(delay);
         if (delay > 0) {
            const int waited = PCM.wait(false, std::max(1, int(1000 * audio_latency(delay, PCM.rate()))));
            if (waited < 0) fail(waited);
            continue;
         }
         PCM.drop();
         const int prepared = PCM.prepare();
         if (prepared < 0) { fail(prepared); break; }
         primed = 0;
         running = false;
         const int waited = PCM.wait(false, -1);
         if (waited < 0) fail(waited);
      }
      else {
         auto available = PCM.available();
         if (available < 0) {
            if (!recover(available)) break;
            const int waited = PCM.wait(false, 5);
            if (waited < 0) fail(waited);
            continue;
         }
         auto budget = PCM.buffer_frames();
         while (!PCM.stopping() and budget and
                available >= int64_t(period.Remaining ? period.Remaining : PCM.period_frames())) {
            if (!period.Remaining) {
               if (!PCM.produce(running)) break;
               period.produce(PCM.period_frames());
            }
            const auto written = PCM.write(period.Offset, period.Remaining);
            if (written < 0) { recover(written); break; }
            if (!written) break;
            if (!period.accept(written)) { fail(-EIO); break; }
            primed += written;
            budget -= std::min(budget, uint64_t(written));
            available -= written;
            if (!running and primed >= PCM.buffer_frames() - PCM.period_frames()) {
               if (!PCM.running()) {
                  const int started = PCM.start();
                  if (started < 0) { recover(started); break; }
               }
               running = true;
            }
         }
         if (PCM.stopping() or failed) break;
         if (running) stats.observe(PCM.delay());
         const int waited = PCM.wait(true, 100);
         if (waited < 0) fail(waited);
      }
   }
   PCM.drop();
   return stats;
}
