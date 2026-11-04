#pragma once

#include <array>

// Caller holds MixerMutex.  Callback ownership is transferred in and out; only the client pins/releases callbacks.
class AudioCompletions {
public:
   struct Entry {
      FUNCTION Callback;
      size_t Remaining = 0;
      ERR Error = ERR::Okay;
      int FailedCommand = -1;
      bool Used = false;
   };

private:
   std::array<Entry, 1024> entries;
   std::array<int, 1024> ready;
   size_t head = 0;
   size_t ready_count = 0;
   size_t count = 0;

public:
   int reserve(size_t Commands, const FUNCTION &Callback) {
      for (size_t i = 0; i < entries.size(); ++i) {
         if (entries[i].Used) continue;
         entries[i] = { Callback, Commands, ERR::Okay, -1, true };
         ++count;
         return int(i);
      }
      return -1;
   }

   void complete(int Slot, int Index, ERR Error) {
      if (Slot < 0) return;
      auto &entry = entries[Slot];
      if (!entry.Remaining) return;
      if (entry.Error IS ERR::Okay and Error != ERR::Okay) {
         entry.Error = Error;
         entry.FailedCommand = Index;
      }
      if (!--entry.Remaining) {
         ready[(head + ready_count) % ready.size()] = Slot;
         ++ready_count;
      }
   }

   bool take(Entry &Result) {
      if (!ready_count) return false;
      const auto slot = ready[head];
      head = (head + 1) % ready.size();
      --ready_count;
      Result = entries[slot];
      entries[slot] = {};
      --count;
      return true;
   }

   size_t pending() const { return count; }
   size_t available() const { return ready_count; }

   template<typename Release> void clear(Release ReleaseCallback) {
      for (auto &entry : entries) {
         if (entry.Used) ReleaseCallback(entry.Callback);
         entry = {};
      }
      head = ready_count = count = 0;
   }
};
