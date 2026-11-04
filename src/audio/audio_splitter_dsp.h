#pragma once

// Parallel branch host for the AudioSplitter processor.
//
// The input is processed in sub-blocks of SPLITTER_BLOCK frames, so that scratch storage is independent of the mix
// buffer size.  For each sub-block, every branch runs its effect chain in place on a copy of the input.  The branch
// output is then delayed by the difference between the container's latency budget and the branch latency, so that
// every branch emerges aligned, and is added to the output at the branch gain.
//
// Each branch occupies a slot that owns its alignment ring and gain state.  A removed branch keeps its slot while its
// gain ramps out, running the processors that were captured from its effects at removal.  The processor accesses
// branch chains and their effects, which is permitted for containers on the condition that the mixer lock is held.

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <span>
#include <vector>

constexpr int SPLITTER_MAX_BRANCHES = 8;
constexpr int SPLITTER_SLOTS = SPLITTER_MAX_BRANCHES * 2; // Active branches and branches fading out after removal
constexpr int SPLITTER_BLOCK = 256;

struct SplitterBranchSettings {
   double Gain = 0;   // Decibels
   bool Mute = true;
};

// The linear gain of a branch in the sum.

inline double splitter_gain(const SplitterBranchSettings &Settings)
{
   return Settings.Mute ? 0.0 : std::pow(10.0, Settings.Gain / 20.0);
}

// A branch edit, prepared on the control thread.  Publication moves retired storage into Trash so that it is
// released after the mixer lock.

struct SplitterEdit {
   std::vector<int> Origins;   // Previous index of each branch, or -1 for a new branch
   std::vector<double> Gains;  // Linear target gain of each branch
   std::vector<std::vector<std::shared_ptr<AudioEffectProcessor>>> Captures; // Reserved, one per previous branch
   std::vector<std::vector<std::shared_ptr<AudioEffectProcessor>>> Trash;    // Reserved for SPLITTER_SLOTS
};

//********************************************************************************************************************

class SplitterProcessor final : public AudioEffectProcessor {
public:
   enum class SLOT : int8_t { FREE, ACTIVE, RETIRING, DONE };

   struct Slot {
      std::weak_ptr<AudioEffectChain> Chain; // Active branches only; the container holds the strong reference
      std::vector<std::shared_ptr<AudioEffectProcessor>> Retired; // Processors of a removed branch
      std::vector<float> Delay;  // Alignment ring of ring_frames frames
      int Cursor = 0;            // Ring frame that receives the next input
      int Lag = 0;               // Alignment delay in frames
      int PreviousLag = 0;       // Alignment delay that is being crossfaded out
      int LagFade = 0;           // Frames remaining in the alignment crossfade
      int Silent = 0;            // Consecutive silent frames most recently written to the ring
      double Gain = 0, Target = 0, Step = 0;
      int RampLeft = 0;
      SLOT State = SLOT::FREE;
   };

   extAudioEffect *Owner;
   double Budget;    // Latency budget in milliseconds; fixed for the lifetime of the processor
   int Rate = 0;
   int Channels = 1;

private:
   std::array<Slot, SPLITTER_SLOTS> slots;
   std::array<int, SPLITTER_MAX_BRANCHES> order = {}; // Slot of each active branch, in branch order
   int count = 0;

   // Storage prepared for storage_rate and storage_channels, swapped in by Configuration::publish().
   std::vector<float> dry, scratch;
   int storage_rate = 0, storage_channels = 0;
   int budget = 0;        // Latency budget in frames at storage_rate
   int ring_frames = 1;   // budget + 1
   int fade_frames = 1;   // 10 ms
   bool active = false;

   // Alignment delay for a branch: the budget less the latency of its non-bypassed effects.

   int target_lag(const AudioEffectChain &Chain) const {
      int64_t latency = 0;
      for (auto effect : Chain.Effects) latency += std::max(int64_t(0), effect->latency());
      return int(std::clamp(int64_t(budget) - latency, int64_t(0), int64_t(budget)));
   }

   // A branch is audible unless it is muted and its gain has finished ramping out.

   static bool audible(const Slot &Slot) {
      return (Slot.Gain != 0) or (Slot.Target != 0) or Slot.RampLeft;
   }

   void clear(Slot &Slot) {
      if (Slot.Silent < ring_frames) std::fill(Slot.Delay.begin(), Slot.Delay.end(), 0.0f);
      Slot.Cursor = 0;
      Slot.LagFade = 0;
      Slot.Silent = ring_frames;
   }

   void ramp(Slot &Slot, double Target, bool Live) {
      Slot.Target = Target;
      if (Live and (Slot.Gain != Target)) {
         Slot.Step = (Target - Slot.Gain) / double(fade_frames);
         Slot.RampLeft = fade_frames;
      }
      else {
         Slot.Gain = Target;
         Slot.RampLeft = 0;
      }
   }

   // Run one sub-block of a branch from the dry input and add its aligned output to Output.

   void run(Slot &Slot, float *Output, int Frames) {
      const int samples = Frames * Channels;
      std::copy_n(dry.data(), samples, scratch.data());

      if (Slot.State IS SLOT::ACTIVE) {
         if (auto chain = Slot.Chain.lock()) {
            process_effects(*chain, scratch.data(), Frames);
            const int target = target_lag(*chain);
            if ((target != Slot.Lag) and (not Slot.LagFade)) {
               Slot.PreviousLag = Slot.Lag;
               Slot.Lag = target;
               Slot.LagFade = fade_frames;
            }
         }
      }
      else for (auto &processor : Slot.Retired) processor->process(scratch.data(), Frames);

      float *ring = Slot.Delay.data();
      for (int frame = 0; frame < Frames; ++frame) {
         const float *input = scratch.data() + size_t(frame) * Channels;
         float *slot = ring + size_t(Slot.Cursor) * Channels;
         bool silent = true;
         for (int c = 0; c < Channels; ++c) {
            slot[c] = input[c];
            silent &= input[c] IS 0;
         }
         Slot.Silent = silent ? std::min(Slot.Silent + 1, ring_frames) : 0;

         int read = Slot.Cursor - Slot.Lag;
         if (read < 0) read += ring_frames;
         const float *delayed = ring + size_t(read) * Channels;
         float *output = Output + size_t(frame) * Channels;

         if (Slot.LagFade) {
            int previous = Slot.Cursor - Slot.PreviousLag;
            if (previous < 0) previous += ring_frames;
            const float *old = ring + size_t(previous) * Channels;
            const double blend = 1.0 - double(Slot.LagFade) / double(fade_frames);
            for (int c = 0; c < Channels; ++c) {
               output[c] += float(Slot.Gain * (old[c] + (delayed[c] - old[c]) * blend));
            }
            --Slot.LagFade;
         }
         else if (Slot.Gain != 0) {
            for (int c = 0; c < Channels; ++c) output[c] += float(Slot.Gain * delayed[c]);
         }

         if (++Slot.Cursor IS ring_frames) Slot.Cursor = 0;
         if (Slot.RampLeft) {
            if (--Slot.RampLeft) Slot.Gain += Slot.Step;
            else Slot.Gain = Slot.Target;
         }
      }

      if ((Slot.State IS SLOT::RETIRING) and (not Slot.RampLeft)) Slot.State = SLOT::DONE;
   }

public:
   class Configuration final : public AudioEffectConfiguration {
   public:
      SplitterProcessor *Processor;
      std::array<std::vector<float>, SPLITTER_SLOTS> Delays;
      std::vector<float> Dry, Scratch;
      int Rate = 0, Channels = 0, Budget = 0;

      explicit Configuration(SplitterProcessor *Target) : Processor(Target) { }

      // Swap under the mixer lock.  The processor's previous storage is retired here and freed after unlocking.
      void publish() override {
         auto &p = *Processor;
         for (int s = 0; s < SPLITTER_SLOTS; s++) p.slots[s].Delay.swap(Delays[s]);
         p.dry.swap(Dry);
         p.scratch.swap(Scratch);
         p.storage_rate = Rate;
         p.storage_channels = Channels;
         p.budget = Budget;
         p.ring_frames = Budget + 1;
         p.active = false;
         for (auto &slot : p.slots) slot.Silent = 0; // New storage is cleared by reset()
      }

      int64_t latency() const override { return Budget; }
   };

   // Branches are listed in branch order with their linear gains.

   SplitterProcessor(extAudioEffect *Effect, double BudgetMs,
      std::span<const std::shared_ptr<AudioEffectChain>> Branches, std::span<const double> Gains)
      : Owner(Effect), Budget(BudgetMs) {
      count = int(std::min(Branches.size(), size_t(SPLITTER_MAX_BRANCHES)));
      for (int i = 0; i < count; i++) {
         order[i] = i;
         slots[i].Chain = Branches[i];
         slots[i].State = SLOT::ACTIVE;
         slots[i].Gain = slots[i].Target = Gains[i];
      }
   }

   // Storage depends only on the rate, channel count and budget, so branch edits never allocate DSP storage.  A zero
   // rate leaves the processor inactive, with no latency, until the output is configured.

   ERR prepare(int PrepareRate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) override {
      auto config = std::make_unique<Configuration>(this);
      if (PrepareRate > 0) {
         const auto frames = budget_frames(Budget, PrepareRate);
         if (frames > 0x7fffffff - 1) return ERR::OutOfRange;
         config->Rate = PrepareRate;
         config->Channels = Stereo ? 2 : 1;
         config->Budget = int(frames);
         for (auto &delay : config->Delays) delay.assign((frames + 1) * config->Channels, 0.0f);
         config->Dry.assign(size_t(SPLITTER_BLOCK) * config->Channels, 0.0f);
         config->Scratch.assign(size_t(SPLITTER_BLOCK) * config->Channels, 0.0f);
      }
      Result = std::move(config);
      return ERR::Okay;
   }

   // Caller holds the mixer lock.  Branches lists the chain of every branch in the new branch order.  Retained
   // branches keep their slots and DSP state.  New branches start at zero gain and ramp in; removed branches ramp out
   // while their captured processors continue to run.  Edits apply without transitions while the processor is
   // inactive or awaiting a reset.

   void update(SplitterEdit &Edit, std::span<const std::shared_ptr<AudioEffectChain>> Branches) {
      for (auto &slot : slots) {
         if (slot.State IS SLOT::DONE) {
            Edit.Trash.push_back(std::move(slot.Retired));
            slot.State = SLOT::FREE;
         }
      }

      const bool live = active and (not Owner->ResetPending);
      std::array<bool, SPLITTER_MAX_BRANCHES> kept = {};
      for (auto origin : Edit.Origins) {
         if ((origin >= 0) and (origin < count)) kept[origin] = true;
      }

      for (int i = 0; i < count; i++) {
         if (kept[i]) continue;
         auto &slot = slots[order[i]];
         auto chain = slot.Chain.lock();
         slot.Chain.reset();
         if (live and chain and (size_t(i) < Edit.Captures.size()) and audible(slot)) {
            slot.Retired.swap(Edit.Captures[i]);
            for (auto effect : chain->Effects) {
               if ((effect->Flags & AEF::BYPASS) != AEF::NIL) continue;
               if (effect->processor and (slot.Retired.size() < slot.Retired.capacity())) {
                  slot.Retired.push_back(effect->processor);
               }
            }
            slot.State = SLOT::RETIRING;
            ramp(slot, 0, true);
         }
         else slot.State = SLOT::FREE;
      }

      std::array<int, SPLITTER_MAX_BRANCHES> next = {};
      const int next_count = int(std::min(Branches.size(), size_t(SPLITTER_MAX_BRANCHES)));
      for (int i = 0; i < next_count; i++) {
         const int origin = (size_t(i) < Edit.Origins.size()) ? Edit.Origins[i] : -1;
         if ((origin >= 0) and (origin < count)) {
            next[i] = order[origin];
            ramp(slots[next[i]], Edit.Gains[i], live);
            continue;
         }

         // A new branch takes a free slot, or the slot of the oldest fading branch if every slot is in use.

         int s = 0;
         while ((s < SPLITTER_SLOTS) and (slots[s].State != SLOT::FREE)) s++;
         if (s IS SPLITTER_SLOTS) {
            s = 0;
            while ((s < SPLITTER_SLOTS) and (slots[s].State != SLOT::RETIRING)) s++;
            Edit.Trash.push_back(std::move(slots[s].Retired));
         }

         auto &slot = slots[s];
         slot.Chain = Branches[i];
         slot.State = SLOT::ACTIVE;
         slot.Gain = 0;
         if (not slot.Delay.empty()) clear(slot);
         slot.Lag = slot.PreviousLag = Branches[i] ? target_lag(*Branches[i]) : budget;
         ramp(slot, Edit.Gains[i], live);
         next[i] = s;
      }

      order = next;
      count = next_count;
   }

   // Buffered alignment and branch tails keep the container pending.  Muted branches that have finished ramping out
   // do not contribute to the output, so their tails are ignored.

   bool pending() const override {
      if (not active) return false;
      for (const auto &slot : slots) {
         if (slot.State IS SLOT::RETIRING) return true;
         if ((slot.State != SLOT::ACTIVE) or (not audible(slot))) continue;
         const int lag = slot.LagFade ? std::max(slot.Lag, slot.PreviousLag) : slot.Lag;
         if (slot.Silent < lag) return true;
         if (auto chain = slot.Chain.lock(); chain and chain->pending()) return true;
      }
      return false;
   }

   AudioTail tail() const override {
      if (not active) return AudioTail::NONE;
      for (int i = 0; i < count; i++) {
         const auto &slot = slots[order[i]];
         if (not audible(slot)) continue;
         auto chain = slot.Chain.lock();
         if (chain and (chain->tail_bound() IS UINT64_MAX)) return AudioTail::INDEFINITE;
      }
      return tail_frames() ? AudioTail::FINITE : AudioTail::NONE;
   }

   // The longest branch tail plus its alignment delay.  Fading branches finish within their ramp and alignment.

   uint64_t tail_frames() const override {
      if (not active) return 0;
      uint64_t frames = 0;
      for (const auto &slot : slots) {
         if (slot.State IS SLOT::RETIRING) {
            frames = std::max(frames, uint64_t(slot.RampLeft) + uint64_t(slot.Lag));
            continue;
         }
         if ((slot.State != SLOT::ACTIVE) or (not audible(slot))) continue;
         uint64_t tail = 0;
         if (auto chain = slot.Chain.lock()) tail = chain->tail_bound();
         const uint64_t lag = uint64_t(slot.LagFade ? std::max(slot.Lag, slot.PreviousLag) : slot.Lag);
         frames = std::max(frames, (tail > UINT64_MAX - lag) ? UINT64_MAX : tail + lag);
      }
      return frames;
   }

   // The greatest branch decay, excluding latency.  The parent chain adds the container's own latency.

   uint64_t decay_estimate() const override {
      if (not active) return 0;
      uint64_t frames = 0;
      for (int i = 0; i < count; i++) {
         const auto &slot = slots[order[i]];
         if (not audible(slot)) continue;
         auto chain = slot.Chain.lock();
         if (not chain) continue;
         uint64_t branch = 0;
         for (auto effect : chain->Effects) {
            if ((not effect->processor) or ((effect->Flags & AEF::BYPASS) != AEF::NIL)) continue;
            const auto estimate = effect->processor->decay_estimate();
            branch = (estimate > UINT64_MAX - branch) ? UINT64_MAX : branch + estimate;
         }
         frames = std::max(frames, branch);
      }
      return frames;
   }

   int64_t latency() const override { return (storage_rate > 0) ? budget : 0; }

   void skip(int Frames) override {
      for (int i = 0; i < count; i++) {
         if (auto chain = slots[order[i]].Chain.lock()) {
            for (auto effect : chain->Effects) effect->skip(Frames);
         }
      }
   }

   // Discards alignment history and fading branches, applies target gains without ramps and idles every branch
   // effect so that their meters publish a final interval and their processors reset.

   void reset() override {
      Rate = Owner->OutputRate;
      Channels = Owner->Stereo ? 2 : 1;
      active = (storage_rate > 0) and (Rate IS storage_rate) and (Channels <= storage_channels) and
         (Owner->Layout.size() <= 2);
      fade_frames = std::max(1, Rate / 100);

      for (auto &slot : slots) {
         if (slot.State IS SLOT::RETIRING) slot.State = SLOT::DONE;
         if (not slot.Delay.empty()) clear(slot);
         slot.Gain = slot.Target;
         slot.RampLeft = 0;
      }

      for (int i = 0; i < count; i++) {
         auto &slot = slots[order[i]];
         auto chain = slot.Chain.lock();
         if (not chain) continue;
         slot.Lag = slot.PreviousLag = active ? target_lag(*chain) : 0;
         for (auto effect : chain->Effects) effect->idle();
      }
   }

   // An inactive processor passes audio through unchanged.

   void process(float *Buffer, int Frames) override {
      if (not active) return;
      while (Frames > 0) {
         const int frames = std::min(Frames, SPLITTER_BLOCK);
         const int samples = frames * Channels;
         std::copy_n(Buffer, samples, dry.data());
         std::fill_n(Buffer, samples, 0.0f);
         for (int i = 0; i < count; i++) run(slots[order[i]], Buffer, frames);
         for (auto &slot : slots) {
            if (slot.State IS SLOT::RETIRING) run(slot, Buffer, frames);
         }
         Buffer += samples;
         Frames -= frames;
      }
   }

   // Diagnostics for unit tests.
   int branch_count() const { return count; }
   const Slot & branch_slot(int Index) const { return slots[order[Index]]; }
   int retiring() const {
      int total = 0;
      for (const auto &slot : slots) total += (slot.State IS SLOT::RETIRING) ? 1 : 0;
      return total;
   }
};
