#pragma once

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

enum class RST : char { ALWAYS, WHEN_NOT_ACTIVE, NEVER };
enum class ANIM_STATE : char { BEFORE, ACTIVE, AFTER };

// Source timing is kept separately from the state derived for a particular document time.

struct anim_timeline {
   struct syncbase {
      std::string id;
      double offset = 0;
      bool end = false;
   };

   std::string id;
   std::vector<double> begin_series;
   std::vector<syncbase> begin_refs;
   std::vector<double> event_times;
   std::vector<double> resolved_begin;
   std::vector<double> resolved_end;
   double begin_offset = 0;
   double duration = -1; // A missing or indefinite simple duration has no interpolation progress.
   double repeat_count = 0; // Zero means unspecified; negative means indefinite.
   double repeat_duration = 0; // Zero means unspecified; negative means indefinite.
   double min_duration = 0;
   double max_duration = std::numeric_limits<double>::infinity();
   double start_time = 0;
   double end_time = 0;
   double seek = 0;
   int repeat_index = 0;
   RST restart = RST::ALWAYS;
   ANIM_STATE state = ANIM_STATE::BEFORE;
   bool freeze = false;
   bool cyclic = false;
   bool effect_ended = false;

   double repeating_duration() const {
      const double simple = (duration < 0) ? std::numeric_limits<double>::infinity() : duration;
      double active = simple;
      if (repeat_count < 0) active = simple ? std::numeric_limits<double>::infinity() : 0;
      else if (repeat_count > 0) active = simple * repeat_count;
      else if (repeat_duration != 0) active = simple ? std::numeric_limits<double>::infinity() : 0;
      if (repeat_duration > 0) active = std::min(active, repeat_duration);
      return active;
   }

   double active_duration() const {
      // A minimum extends the interval used for restart and syncbase timing, without extending the effect.
      double active = repeating_duration();
      if (min_duration <= max_duration) active = std::clamp(active, min_duration, max_duration);
      return active;
   }

   double effect_duration() const {
      return std::min(repeating_duration(), active_duration());
   }

   void evaluate(double Time) {
      start_time = end_time = seek = 0;
      repeat_index = 0;
      state = ANIM_STATE::BEFORE;
      effect_ended = false;
      const auto next = std::upper_bound(resolved_begin.begin(), resolved_begin.end(), Time);
      if (next IS resolved_begin.begin()) return;

      start_time = *std::prev(next);
      double elapsed = Time - start_time;
      const double active = active_duration();
      const double tolerance = 8 * std::numeric_limits<double>::epsilon() * std::max(std::abs(elapsed), active);
      if (std::isfinite(active) and (std::abs(elapsed - active) <= tolerance)) {
         elapsed = active;
      }
      const double effect = effect_duration();
      const double effect_tolerance = 8 * std::numeric_limits<double>::epsilon() * std::max(std::abs(elapsed), effect);
      if (std::isfinite(effect) and (std::abs(elapsed - effect) <= effect_tolerance)) elapsed = effect;
      state = (elapsed < active) ? ANIM_STATE::ACTIVE : ANIM_STATE::AFTER;
      effect_ended = elapsed >= effect;
      if (state IS ANIM_STATE::AFTER) end_time = start_time + active;
      if (duration < 0) return;
      if (!duration) { seek = 1; return; }

      const double sample = std::min(elapsed, effect);
      double cycles = sample / duration;
      if (!std::isfinite(cycles)) {
         seek = std::fmod(sample, duration) / duration;
         repeat_index = std::numeric_limits<int>::max();
         return;
      }
      const double nearest = std::round(cycles);
      if (std::abs(cycles - nearest) <= 8 * std::numeric_limits<double>::epsilon() * std::max(1.0, cycles)) {
         cycles = nearest;
      }
      double index = std::floor(cycles);
      seek = cycles - index;
      if (effect_ended and (sample > 0) and (seek IS 0)) {
         seek = 1;
         index--;
      }
      repeat_index = int(std::clamp(index, 0.0, double(std::numeric_limits<int>::max())));
   }

   bool contributes() const {
      return (state != ANIM_STATE::BEFORE) and (!effect_ended or freeze);
   }

   void record_event(double Time) {
      if (std::isfinite(Time)) event_times.push_back(Time);
   }

   bool discard_events_after(double Time) {
      const auto size = event_times.size();
      std::erase_if(event_times, [Time](double Instance) { return Instance > Time; });
      return size != event_times.size();
   }
};

// Resolve dependencies before their consumers.  The explicit DFS stack also bounds native stack use for long chains.
// A cycle invalidates all its members, including their otherwise independent offset instances.

inline void resolve_animation_times(const std::vector<anim_timeline *> &Animations)
{
   std::unordered_map<std::string, anim_timeline *> ids;
   std::unordered_map<anim_timeline *, unsigned char> visits;
   for (auto anim : Animations) {
      anim->resolved_begin.clear();
      anim->resolved_end.clear();
      anim->cyclic = false;
      if (!anim->id.empty()) ids.emplace(anim->id, anim);
   }

   struct frame { anim_timeline *anim; size_t ref = 0; };
   std::vector<frame> stack;
   for (auto root : Animations) {
      if (visits[root]) continue;
      visits[root] = 1;
      stack.push_back({ root });
      while (!stack.empty()) {
         auto &entry = stack.back();
         auto anim = entry.anim;
         if (entry.ref < anim->begin_refs.size()) {
            const auto &ref = anim->begin_refs[entry.ref++];
            const auto found = ids.find(ref.id);
            if (found IS ids.end()) continue;
            auto source = found->second;
            if (!visits[source]) {
               visits[source] = 1;
               stack.push_back({ source });
            }
            else if (visits[source] IS 1) {
               bool member = false;
               for (auto &item : stack) {
                  if (item.anim IS source) member = true;
                  if (member) item.anim->cyclic = true;
               }
            }
            continue;
         }

         if (!anim->cyclic) {
            auto &begins = anim->resolved_begin;
            if (anim->begin_series.empty()) begins.push_back(anim->begin_offset);
            else begins = anim->begin_series;
            begins.insert(begins.end(), anim->event_times.begin(), anim->event_times.end());
            for (auto &ref : anim->begin_refs) {
               const auto source = ids.find(ref.id);
               if (source IS ids.end()) continue;
               const auto &instances = ref.end ? source->second->resolved_end : source->second->resolved_begin;
               for (double instance : instances) begins.push_back(instance + ref.offset);
            }
            std::erase_if(begins, [](double Time) { return !std::isfinite(Time); });
            std::sort(begins.begin(), begins.end());
            begins.erase(std::unique(begins.begin(), begins.end()), begins.end());

            const double active = anim->active_duration();
            if ((anim->restart IS RST::NEVER) and (begins.size() > 1)) begins.resize(1);
            else if (anim->restart IS RST::WHEN_NOT_ACTIVE) {
               size_t count = 0;
               for (double begin : begins) {
                  if (count and (begin < begins[count-1] + active)) continue;
                  begins[count++] = begin;
               }
               begins.resize(count);
            }
            for (size_t i=0; i < begins.size(); i++) {
               double end = begins[i] + active;
               if (i+1 < begins.size()) end = std::min(end, begins[i+1]);
               anim->resolved_end.push_back(end);
            }
         }
         visits[anim] = 2;
         stack.pop_back();
      }
   }
}
