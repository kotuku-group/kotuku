// Copyright © 2025-2026 Paul Manias

#pragma once

#include <kotuku/main.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

struct ParserProfilingStage {
   std::string name;
   double milliseconds = 0.0;
};

struct ParserProfilingResult {
   void clear();
   void record_stage(std::string_view name, double milliseconds);
   const std::vector<ParserProfilingStage> & stages() const;
   std::vector<ParserProfilingStage> & stages();
   bool empty() const;
   void log_results(pf::Log& log) const;

private:
   std::vector<ParserProfilingStage> entries;
};

class ParserProfiler {
public:
   class StageTimer {
   public:
      StageTimer(ParserProfiler* profiler, std::string_view name);
      StageTimer(StageTimer&& other) noexcept;
      StageTimer& operator=(StageTimer&& other) noexcept;
      StageTimer(const StageTimer&) = delete;
      StageTimer& operator=(const StageTimer&) = delete;
      ~StageTimer();

      void stop();

   private:
      void reset();

      ParserProfiler* profiler = nullptr;
      std::string stage_name;
      std::chrono::steady_clock::time_point start_time;
      bool running = false;
   };

   ParserProfiler(bool, ParserProfilingResult *);

   StageTimer stage(std::string_view);
   void record_stage(std::string_view, std::chrono::steady_clock::duration);
   void log_results(pf::Log &) const;
   bool enabled() const;

private:
   void store(std::string_view name, double milliseconds);

   bool is_enabled = false;
   ParserProfilingResult *payload = nullptr;
};

inline void ParserProfilingResult::clear()
{
   this->entries.clear();
}

inline void ParserProfilingResult::record_stage(std::string_view name, double milliseconds)
{
   ParserProfilingStage stage;
   stage.name.assign(name.data(), name.size());
   stage.milliseconds = milliseconds;
   this->entries.push_back(std::move(stage));
}

inline const std::vector<ParserProfilingStage>& ParserProfilingResult::stages() const
{
   return this->entries;
}

inline std::vector<ParserProfilingStage>& ParserProfilingResult::stages()
{
   return this->entries;
}

inline bool ParserProfilingResult::empty() const
{
   return this->entries.empty();
}

inline void ParserProfilingResult::log_results(pf::Log &log) const
{
   if (this->entries.empty()) return;

   for (const ParserProfilingStage& stage : this->entries) {
      log.msg("profile-stage[%s] = %.3fms", stage.name.c_str(), stage.milliseconds);
   }
}

inline ParserProfiler::StageTimer::StageTimer(ParserProfiler* profiler, std::string_view name)
   : profiler(profiler)
{
   if ((this->profiler) and (this->profiler->enabled())) {
      this->stage_name.assign(name.data(), name.size());
      this->start_time = std::chrono::steady_clock::now();
      this->running = true;
   }
}

inline ParserProfiler::StageTimer::StageTimer(StageTimer&& other) noexcept
{
   *this = std::move(other);
}

inline ParserProfiler::StageTimer& ParserProfiler::StageTimer::operator=(StageTimer&& other) noexcept
{
   if (this IS &other) return *this;

   this->stop();
   this->profiler = other.profiler;
   this->stage_name = std::move(other.stage_name);
   this->start_time = other.start_time;
   this->running = other.running;
   other.profiler = nullptr;
   other.running = false;
   return *this;
}

inline ParserProfiler::StageTimer::~StageTimer()
{
   this->stop();
}

inline void ParserProfiler::StageTimer::stop()
{
   if ((not this->running) or (not this->profiler)) return;

   auto end_time = std::chrono::steady_clock::now();
   auto duration = end_time - this->start_time;
   this->profiler->record_stage(this->stage_name, duration);
   this->reset();
}

inline void ParserProfiler::StageTimer::reset()
{
   this->running = false;
   this->profiler = nullptr;
   this->stage_name.clear();
}

inline ParserProfiler::ParserProfiler(bool enabled, ParserProfilingResult *result)
{
   if ((enabled) and (result)) {
      this->is_enabled = true;
      this->payload = result;
      this->payload->clear();
   }
}

inline ParserProfiler::StageTimer ParserProfiler::stage(std::string_view name)
{
   if (not this->is_enabled) return StageTimer(nullptr, std::string_view{});
   return StageTimer(this, name);
}

inline void ParserProfiler::record_stage(std::string_view name, std::chrono::steady_clock::duration duration)
{
   if (not this->is_enabled) return;
   auto millis = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(duration);
   this->store(name, millis.count());
}

inline void ParserProfiler::log_results(pf::Log& log) const
{
   if ((not this->is_enabled) or (not this->payload)) return;
   this->payload->log_results(log);
}

inline bool ParserProfiler::enabled() const
{
   return this->is_enabled;
}

inline void ParserProfiler::store(std::string_view name, double milliseconds)
{
   if ((not this->payload) or (not this->is_enabled)) return;
   this->payload->record_stage(name, milliseconds);
}

