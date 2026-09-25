#pragma once

#include <atomic>
#include <deque>
#include <future>
#include <thread>
#include "../audio_batch.h"
#ifdef _WIN32
#include "../audio_endpoint.h"
#endif

// Each invocation owns its counters; transport callbacks may report checks from worker threads.
struct AudioTestContext {
   std::atomic<int> Passed{0};
   std::atomic<int> Total{0};

   bool check(bool Condition, const char *Expression, const char *File, int Line) {
      ++Total;
      if (Condition) ++Passed;
      else kt::Log("AudioTests").warning("%s:%d: %s", File, Line, Expression);
      return Condition;
   }
};

// Keep side effects active in every build, and report failures without terminating the host process.
#define AUDIO_CHECK(...) Test.check(bool(__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define AUDIO_REQUIRE(...) do { if (!AUDIO_CHECK(__VA_ARGS__)) return; } while (false)
