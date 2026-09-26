// Internal effect-chain operations. All DSP and chain mutations share the owning mixer mutex.

static void sort_effects(AudioEffectChain &Chain)
{
   std::sort(Chain.Effects.begin(), Chain.Effects.end(), [](auto Left, auto Right) {
      if (Left->Order != Right->Order) return Left->Order < Right->Order;
      return Left->Sequence < Right->Sequence;
   });
}

bool extAudioEffect::pending() const
{
   return processor and !ResetPending and ((Flags & AEF::BYPASS) IS AEF::NIL) and processor->pending();
}

int64_t extAudioEffect::latency() const
{
   return (Flags & AEF::BYPASS) != AEF::NIL ? 0 : CommittedLatency;
}

bool AudioEffectChain::pending() const
{
   for (auto effect : Effects) if (effect->pending()) return true;
   return false;
}

static bool effects_pending(const AudioEffectChain &Chain)
{
   return Chain.pending();
}

ERR AudioEffectChain::latency(int64_t &Frames) const
{
   Frames = 0;
   for (auto effect : Effects) {
      const auto frames = effect->latency();
      if (frames < 0 or frames > INT64_MAX - Frames) return ERR::OutOfRange;
      Frames += frames;
   }
   return ERR::Okay;
}

uint64_t AudioEffectChain::tail_bound() const
{
   uint64_t frames = 0;
   for (auto effect : Effects) {
      if (!effect->processor or (effect->Flags & AEF::BYPASS) != AEF::NIL) continue;
      if (effect->processor->tail() IS AudioTail::INDEFINITE) return UINT64_MAX;
      const auto tail = effect->processor->tail_frames();
      if (tail > UINT64_MAX - frames) return UINT64_MAX;
      frames += tail;
   }
   return frames;
}

void extAudioEffect::reset_meter(uint64_t Generation)
{
   const auto sequence = Meter.Sequence;
   Meter = {};
   Meter.Sequence = sequence;
   Meter.Generation = Generation;
   MeterFrames = 0;
   MeterPosition = 0;
   Peaks = {};
}

// Snapshot storage is fixed-size and is copied under the mixer mutex. Serialisation happens after unlocking.
void extAudioEffect::publish_meter(AMF Flags)
{
   if (!MeterFrames) return;
   ++Meter.Sequence;
   Meter.Position = MeterPosition;
   Meter.Interval = MeterFrames;
   Meter.Flags = Flags | AMF::VALID; // Flags also identify final/stale intervals.
   Meter.Floor = 0;
   for (int i = 0; i < 4; ++i) {
      if (Peaks[i] < 1e-6) Meter.Floor |= 1 << i;
      Meter.Values[i] = 20 * std::log10(std::max(Peaks[i], 1e-6));
   }
   Meter.Values[4] = Peaks[4];
   MeterFrames = 0;
   Peaks = {};
}

void extAudioEffect::idle()
{
   publish_meter(AMF::IDLE);
   Meter.Flags |= AMF::IDLE;
   if (processor) processor->reset();
   ResetPending = false;
}

void AudioEffectChain::reset()
{
   ++*Generation;
   for (auto effect : Effects) {
      effect->ResetPending = true;
      effect->reset_meter(*Generation);
   }
   State = ADS::IDLE;
   DrainFrames = 0;
   Truncated = false;
}

void extAudioEffect::process(float *Buffer, int Frames)
{
   if ((Flags & AEF::BYPASS) != AEF::NIL or !processor) return;
   auto chain = Chain.lock();
   if (ResetPending) {
      processor->reset();
      ResetPending = false;
   }
   if (OutputRate < 2) {
      processor->process(Buffer, Frames);
      return;
   }
   if (chain and Meter.Generation != *chain->Generation) reset_meter(*chain->Generation);
   const int channels = Stereo ? 2 : 1;
   const int interval = std::max(1, (OutputRate + 19) / 20); // Ceiling: never shorter than 50 ms.
   const double scale = chain ? chain->MeterScale : 1;
   while (Frames > 0) {
      const int count = std::min(Frames, interval - MeterFrames);
      for (int frame = 0; frame < count; ++frame) {
         for (int c = 0; c < channels; ++c) {
            Peaks[c] = std::max(Peaks[c], std::abs(double(Buffer[frame * channels + c])) / scale);
         }
      }
      processor->process(Buffer, count);
      for (int frame = 0; frame < count; ++frame) {
         for (int c = 0; c < channels; ++c) {
            Peaks[2 + c] = std::max(Peaks[2 + c], std::abs(double(Buffer[frame * channels + c])) / scale);
         }
      }
      // Processors return maximum reduction during the last process() call, never a destructive read.
      Peaks[4] = std::max(Peaks[4], processor->gain_reduction());
      MeterFrames += count;
      MeterPosition += count;
      if (chain) Meter.Generation = *chain->Generation;
      if (MeterFrames IS interval) publish_meter(AMF::NIL);
      Buffer += count * channels;
      Frames -= count;
   }
}

ERR extAudioEffect::set_processor(std::unique_ptr<AudioEffectProcessor> Processor)
{
   if (!Processor) return ERR::NullArgs;
   auto chain = Chain.lock();
   if (!chain) return ERR::NotInitialised;
   for (;;) {
      int rate;
      bool stereo;
      {
         std::lock_guard lock(*chain->Mutex);
         rate = OutputRate;
         stereo = Stereo;
      }
      std::unique_ptr<AudioEffectConfiguration> prepared;
      if (auto error = Processor->prepare(rate, stereo, prepared); error != ERR::Okay) return error;
      const auto latency = prepared ? prepared->latency() : Processor->latency();
      if (latency < 0) return ERR::InvalidValue;
      {
         std::lock_guard mixer_lock(*chain->Mutex);
         if (rate != OutputRate or stereo != bool(Stereo)) continue;
         if (processor) return ERR::InvalidState;
         if (prepared) prepared->publish();
         CommittedLatency = latency;
         processor = std::move(Processor);
         ResetPending = true;
         reset_meter(++*chain->Generation);
      }
      return ERR::Okay;
   }
}

static void process_effects(AudioEffectChain &Chain, float *Buffer, int Frames)
{
   for (auto effect : Chain.Effects) effect->process(Buffer, Frames);
}

// SourceFrames is the last actual source frame in this window, not the last non-zero sample or upstream tail.
// Every serial processor runs during drain, including when an upstream delay currently outputs silence.
static void render_effects(AudioEffectChain &Chain, float *Buffer, int Frames, int SourceFrames, uint64_t Limit,
   bool SourceActive, uint64_t UpstreamBound)
{
   if (Chain.Effects.empty()) {
      if (SourceFrames) {
         Chain.DrainFrames = Frames - SourceFrames;
         Chain.Truncated = false;
      }
      else Chain.DrainFrames = std::min(Limit, Chain.DrainFrames + Frames);
      Chain.State = SourceActive ? ADS::ACTIVE : ADS::IDLE;
      return;
   }
   const int channels = Chain.Stereo ? 2 : 1;
   if (SourceFrames) {
      Chain.DrainFrames = 0;
      Chain.Truncated = false;
      Chain.State = ADS::ACTIVE;
      process_effects(Chain, Buffer, SourceFrames);
      Buffer += SourceFrames * channels;
      Frames -= SourceFrames;
   }
   if (Frames > 0) {
      bool input = false;
      for (int i = 0; i < Frames * channels; ++i) input |= Buffer[i] != 0;
      if (!input and !Chain.pending()) {
         Chain.DrainFrames = std::min(Limit, Chain.DrainFrames + Frames);
         Chain.State = ADS::IDLE;
         for (auto effect : Chain.Effects) {
            if (effect->MeterFrames or (effect->Meter.Flags & AMF::IDLE) IS AMF::NIL) effect->idle();
         }
         return;
      }
      const int count = int(std::min(uint64_t(Frames), Limit - std::min(Limit, Chain.DrainFrames)));
      if (count) {
         process_effects(Chain, Buffer, count);
         const uint64_t fade = std::max(1, (Chain.Rate + 99) / 100);
         const auto bound = Chain.tail_bound();
         const bool needs_fade = bound > Limit or UpstreamBound > Limit - std::min(Limit, bound) or
            Chain.DrainFrames >= bound + UpstreamBound or (Chain.DrainFrames + count >= Limit and Chain.pending());
         for (int frame = 0; frame < count; ++frame) {
            const auto left = Limit - Chain.DrainFrames - frame;
            if (needs_fade and left <= fade) {
               const float gain = float(left - 1) / float(fade);
               for (int c = 0; c < channels; ++c) Buffer[frame * channels + c] *= gain;
            }
         }
         Chain.DrainFrames += count;
      }
      if (Chain.DrainFrames >= Limit) {
         if (Chain.pending()) Chain.Truncated = true;
         std::fill_n(Buffer + count * channels, (Frames - count) * channels, 0.0f);
         for (auto effect : Chain.Effects) effect->idle();
         Chain.State = ADS::IDLE;
      }
      else Chain.State = Chain.pending() ? ADS::DRAINING : ADS::IDLE;
   }
   if (!SourceActive and !Chain.pending()) Chain.State = ADS::IDLE;
   else if (!SourceActive) Chain.State = ADS::DRAINING;
   if (Chain.State IS ADS::IDLE) {
      for (auto effect : Chain.Effects) effect->idle();
   }
}

// Control-thread operation. Hold shared processor ownership across preparation, then revalidate attachment and
// generation before publication. The worker is stopped by the caller for a device configuration change.
static ERR configure_effects(AudioEffectChain &Chain, int OutputRate, bool Stereo)
{
   struct Prepared {
      extAudioEffect *Effect;
      std::shared_ptr<AudioEffectProcessor> Processor;
      std::unique_ptr<AudioEffectConfiguration> Configuration;
   };
   std::vector<Prepared> prepared;
   uint64_t generation;
   {
      std::lock_guard lock(*Chain.Mutex);
      generation = *Chain.Generation;
      for (auto effect : Chain.Effects) prepared.push_back({effect, effect->processor, nullptr});
   }
   for (auto &entry : prepared) {
      if (!entry.Processor) continue;
      auto error = entry.Processor->prepare(OutputRate, Stereo, entry.Configuration);
      if (error != ERR::Okay) return error;
      if ((entry.Configuration ? entry.Configuration->latency() : entry.Processor->latency()) < 0) {
         return ERR::InvalidValue;
      }
   }
   {
      std::lock_guard lock(*Chain.Mutex);
      if (generation != *Chain.Generation) return ERR::InvalidState;
      Chain.Rate = OutputRate;
      Chain.Stereo = Stereo;
      Chain.reset();
      for (auto &entry : prepared) {
         if (entry.Configuration) entry.Configuration->publish();
         entry.Effect->OutputRate = OutputRate;
         entry.Effect->Stereo = Stereo;
         entry.Effect->CommittedLatency = entry.Configuration ? entry.Configuration->latency() :
            (entry.Processor ? entry.Processor->latency() : 0);
      }
   }
   return ERR::Okay;
}

void extAudioEffect::detach()
{
   if (auto chain = Chain.lock()) {
      std::lock_guard mixer_lock(*chain->Mutex);
      std::erase(chain->Effects, this);
      ++*chain->Generation;
   }
   Chain.reset();
}

extAudioEffect::~extAudioEffect()
{
   detach();
}
