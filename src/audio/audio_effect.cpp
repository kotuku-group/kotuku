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

//********************************************************************************************************************

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

//********************************************************************************************************************
// Estimated frames for the chain's output to decay by 60 dB after input ends, including algorithmic latency.

uint64_t AudioEffectChain::decay_estimate() const
{
   uint64_t frames = 0;
   for (auto effect : Effects) {
      if (!effect->processor or (effect->Flags & AEF::BYPASS) != AEF::NIL) continue;
      const auto estimate = effect->processor->decay_estimate() + uint64_t(std::max(int64_t(0), effect->latency()));
      frames = (estimate > UINT64_MAX - frames) ? UINT64_MAX : frames + estimate;
   }
   return frames;
}

//********************************************************************************************************************

static double meter_default(const AudioMeterDesc &Desc)
{
   return Desc.Source IS AudioMeterSource::GAIN_REDUCTION ? 0 : -120;
}

//********************************************************************************************************************
// Control-thread operation, called under the chain lock where one exists.  Rebuilds the meter layout from the schema's
// scalar output templates: channel-scoped outputs are published once per channel in layout order, and global outputs
// once.  The internal descriptors are only replaced if membership or ordering changes.  GetMeterLayout() returns
// an independently owned snapshot of these descriptors.
//
// Clients detect a stale meter layout through the chain generation, so every caller must advance the chain generation
// after calling set_layout().

void extAudioEffect::set_layout(std::span<const int> NewLayout)
{
   Layout.assign(NewLayout.begin(), NewLayout.end());
   Stereo = Layout.size() IS 2;

   std::vector<AudioMeterDesc> meters;
   if (Schema) {
      for (const auto &output : Schema->Outputs) {
         if (output.Kind != AudioOutputKind::SCALAR) continue;

         AudioMeterDesc meter;
         meter.Key         = output.Key;
         meter.Label       = output.Label.empty() ? output.Key : output.Label;
         meter.Unit        = output.Unit;
         meter.Scope       = output.Scope;
         meter.Semantics   = output.Semantics;
         meter.Channel     = 0;
         meter.Description = output.Description;
         meter.Source      = output.Source;

         if (meter.Scope IS "channel") {
            for (size_t c = 0; c < Layout.size(); c++) {
               auto &entry   = meters.emplace_back(meter);
               entry.Key     = meter.Key + "_" + channel_key(Layout[c]);
               entry.Label   = meter.Label + " " + channel_label(Layout[c]);
               entry.Channel = Layout[c];
               entry.Index   = int(c);
            }
         }
         else meters.push_back(std::move(meter));
      }
   }

   for (size_t i = 0; i < meters.size(); i++) meters[i].Slot = int(i);

   bool changed = meters.size() != Meters.size();
   for (size_t i = 0; (not changed) and (i < meters.size()); i++) {
      changed = (meters[i].Key != Meters[i].Key) or (meters[i].Channel != Meters[i].Channel);
   }

   if (changed) Meters.swap(meters);

   InputPeaks.assign(Layout.size(), 0);
   OutputPeaks.assign(Layout.size(), 0);
   Meter.Values.resize(Meters.size());
   Meter.ValueFlags.resize(Meters.size());
   reset_meter(Meter.Generation);
}

//********************************************************************************************************************
// Reset in place.  This can run on the render thread, so it must not allocate or release storage.

void extAudioEffect::reset_meter(uint64_t Generation)
{
   Meter.Generation = Generation;
   Meter.Position   = 0;
   Meter.Interval   = 0;
   Meter.Flags      = AMF::NIL;
   for (size_t i = 0; i < Meters.size(); i++) {
      Meter.Values[i] = meter_default(Meters[i]);
      Meter.ValueFlags[i] = 0;
   }
   MeterFrames = 0;
   MeterPosition = 0;
   std::fill(InputPeaks.begin(), InputPeaks.end(), 0.0);
   std::fill(OutputPeaks.begin(), OutputPeaks.end(), 0.0);
   Reduction = 0;
}

//********************************************************************************************************************
// True if the effect's current snapshot is a measurement that a client may use.  Called under the chain lock.

static bool meter_usable(const extAudioEffect &Effect, const AudioEffectChain *Chain)
{
   return Chain and ((Effect.Flags & AEF::BYPASS) IS AEF::NIL) and ((Effect.Meter.Flags & AMF::VALID) != AMF::NIL);
}

//********************************************************************************************************************
// The generation reported by both ReadMeters() and GetMeterLayout().  Clients re-read the meter layout when the two
// differ, so they must share this definition or the comparison would never settle.  Called under the chain lock.

uint64_t extAudioEffect::meter_generation(const AudioEffectChain *Chain) const
{
   return (meter_usable(*this, Chain) or !Chain) ? Meter.Generation : *Chain->Generation;
}

//********************************************************************************************************************
// Copies the latest snapshot into Reading.  Values of an unusable snapshot are copied without flags.  Called under the
// chain lock.

void extAudioEffect::read_meter(const AudioEffectChain *Chain, MeterReading &Reading) const
{
   const bool valid = meter_usable(*this, Chain);
   if (!Chain) Reading.Flags = AMF::DISCONNECTED;
   else if ((Flags & AEF::BYPASS) != AEF::NIL) Reading.Flags = AMF::BYPASSED;
   else if (!valid) Reading.Flags = Meter.Flags | AMF::NO_SAMPLES;
   else Reading.Flags = Meter.Flags;

   Reading.Values.resize(Meter.Values.size());
   for (size_t i = 0; i < Meter.Values.size(); i++) {
      Reading.Values[i] = { Meter.Values[i], valid ? AMV(Meter.ValueFlags[i]) : AMV::NIL };
   }

   Reading.Sequence   = int64_t(Meter.Sequence);
   Reading.ID         = int64_t(meter_generation(Chain));
   Reading.Position   = int64_t(Meter.Position);
   Reading.Interval   = Meter.Interval;
}

//********************************************************************************************************************
// Snapshot storage is preallocated and is copied under the mixer mutex.  Serialisation happens after unlocking.

void extAudioEffect::publish_meter(AMF Flags)
{
   if (!MeterFrames) return;
   ++Meter.Sequence;
   Meter.Position = MeterPosition;
   Meter.Interval = MeterFrames;
   Meter.Flags = Flags | AMF::VALID; // Flags also identify final/stale intervals.
   for (size_t i = 0; i < Meters.size(); i++) {
      const auto &desc = Meters[i];
      if (desc.Source IS AudioMeterSource::GAIN_REDUCTION) {
         Meter.Values[i] = Reduction;
         Meter.ValueFlags[i] = int(AMV::VALID);
         continue;
      }
      const auto &peaks = desc.Source IS AudioMeterSource::INPUT_PEAK ? InputPeaks : OutputPeaks;
      const double peak = ((desc.Index >= 0) and (size_t(desc.Index) < peaks.size())) ? peaks[desc.Index] : 0;
      Meter.Values[i] = 20 * std::log10(std::max(peak, 1e-6));
      Meter.ValueFlags[i] = int(AMV::VALID) | ((peak < 1e-6) ? int(AMV::FLOOR) : 0);
   }
   MeterFrames = 0;
   std::fill(InputPeaks.begin(), InputPeaks.end(), 0.0);
   std::fill(OutputPeaks.begin(), OutputPeaks.end(), 0.0);
   Reduction = 0;
}

//********************************************************************************************************************

void extAudioEffect::idle()
{
   publish_meter(AMF::IDLE);
   Meter.Flags |= AMF::IDLE;
   if (processor) processor->reset();
   ResetPending = false;
}

//********************************************************************************************************************

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

//********************************************************************************************************************

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
   const int channels = int(Layout.size());
   if (channels < 1) {
      processor->process(Buffer, Frames);
      return;
   }

   const int interval = std::max(1, (OutputRate + 19) / 20); // Ceiling: never shorter than 50 ms.
   while (Frames > 0) {
      const int count = std::min(Frames, interval - MeterFrames);
      for (int frame = 0; frame < count; ++frame) {
         for (int c = 0; c < channels; ++c) {
            InputPeaks[c] = std::max(InputPeaks[c], std::abs(double(Buffer[frame * channels + c])));
         }
      }

      processor->process(Buffer, count);
      for (int frame = 0; frame < count; ++frame) {
         for (int c = 0; c < channels; ++c) {
            OutputPeaks[c] = std::max(OutputPeaks[c], std::abs(double(Buffer[frame * channels + c])));
         }
      }

      // Processors return maximum reduction during the last process() call, never a destructive read.
      Reduction = std::max(Reduction, processor->gain_reduction());
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

//********************************************************************************************************************

static void process_effects(AudioEffectChain &Chain, float *Buffer, int Frames)
{
   for (auto effect : Chain.Effects) effect->process(Buffer, Frames);
}

//********************************************************************************************************************
// SourceFrames is the last actual source frame in this window, not the last non-zero sample or upstream tail.
// Every serial processor runs during drain, including when an upstream delay currently outputs silence.

static void render_effects(AudioEffectChain &Chain, float *Buffer, int Frames, int SourceFrames, uint64_t Limit,
   bool SourceActive, uint64_t UpstreamBound, bool UpstreamPending)
{
   if (Chain.Effects.empty()) {
      if (SourceFrames) {
         Chain.DrainFrames = Frames - SourceFrames;
         Chain.Truncated = false;
      }
      else Chain.DrainFrames = std::min(Limit, Chain.DrainFrames + Frames);
      Chain.State = SourceActive ? ADS::ACTIVE : (UpstreamPending ? ADS::DRAINING : ADS::IDLE);
      return;
   }

   const int channels = std::max(1, int(Chain.Layout.size()));
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
      if (!input and !Chain.pending() and !UpstreamPending) {
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
      else Chain.State = Chain.pending() or UpstreamPending ? ADS::DRAINING : ADS::IDLE;
   }

   if (!SourceActive and !Chain.pending() and !UpstreamPending) Chain.State = ADS::IDLE;
   else if (!SourceActive) Chain.State = ADS::DRAINING;

   if (Chain.State IS ADS::IDLE) {
      for (auto effect : Chain.Effects) effect->idle();
   }
}

//********************************************************************************************************************
// Prepare every chain before publishing any of them.  Shared processor ownership keeps preparation safe if an
// effect is detached in the meantime; the shared generation is checked before dereferencing the effect again.

struct PreparedEffectChain {
   struct Entry {
      extAudioEffect *Effect;
      std::shared_ptr<AudioEffectProcessor> Processor;
      std::unique_ptr<AudioEffectConfiguration> Configuration;
   };

   std::shared_ptr<AudioEffectChain> Chain;
   uint64_t Generation;
   std::vector<Entry> Entries;

   ERR prepare(int OutputRate, bool Stereo) {
      {
         std::lock_guard lock(*Chain->Mutex);
         Generation = *Chain->Generation;
         for (auto effect : Chain->Effects) Entries.push_back({effect, effect->processor, nullptr});
      }
      for (auto &entry : Entries) {
         if (!entry.Processor) continue;
         auto error = entry.Processor->prepare(OutputRate, Stereo, entry.Configuration);
         if (error != ERR::Okay) return error;
         if ((entry.Configuration ? entry.Configuration->latency() : entry.Processor->latency()) < 0) {
            return ERR::InvalidValue;
         }
      }
      return ERR::Okay;
   }

   // Caller holds the mixer lock and has validated every chain before any generation is advanced.

   void publish(int OutputRate, std::span<const int> Layout, uint64_t FormatGeneration) {
      Chain->Rate = OutputRate;
      Chain->Stereo = Layout.size() IS 2;
      Chain->Layout.assign(Layout.begin(), Layout.end());
      Chain->FormatGeneration = FormatGeneration;
      for (auto &entry : Entries) {
         if (entry.Configuration) entry.Configuration->publish();
         entry.Effect->OutputRate = OutputRate;
         entry.Effect->set_layout(Layout);
         entry.Effect->FormatGeneration = FormatGeneration;
         entry.Effect->FormatCommitted = true;
         entry.Effect->CommittedLatency = entry.Configuration ? entry.Configuration->latency() :
            (entry.Processor ? entry.Processor->latency() : 0);
      }
      Chain->reset();
   }
};

//********************************************************************************************************************
// The caller has stopped the old worker and negotiated the candidate device format.  Start must either succeed or
// return without leaving a running worker.  Rendering stays excluded until the complete configuration is published.
// Preparation and startup failures leave the last committed format and every effect's configuration untouched.

static ERR commit_audio_output(extAudio *Self, std::span<const int> Layout, ERR (*Start)(extAudio *))
{
   std::vector<PreparedEffectChain> prepared;
   {
      std::lock_guard lock(Self->MixerMutex);
      prepared.push_back({Self->GlobalEffects, 0, {}});
      for (auto &set : Self->Sets) {
         set.ScratchBuffer.resize(Self->MixBuffer.size());
         if (set.Effects) prepared.push_back({set.Effects, 0, {}});
      }
   }

   for (auto &chain : prepared) {
      if (auto error = chain.prepare(Self->OutputRate, Layout.size() IS 2); error != ERR::Okay) return error;
   }

   std::lock_guard lock(Self->MixerMutex);
   for (const auto &chain : prepared) {
      if (chain.Generation != *chain.Chain->Generation) return ERR::InvalidState;
   }

   if (Start) {
      if (auto error = Start(Self); error != ERR::Okay) return error;
   }

   const auto generation = Self->OutputGeneration + 1;
   for (auto &chain : prepared) chain.publish(Self->OutputRate, Layout, generation);
   Self->CommittedLayout.assign(Layout.begin(), Layout.end());
   Self->CommittedRate = Self->OutputRate;
   Self->OutputGeneration = generation;
   Self->EffectConfigured = true;
   Self->OutputActive = true;
   return ERR::Okay;
}

//********************************************************************************************************************

#ifdef UNIT_TESTS
static ERR configure_effects(AudioEffectChain &Chain, int OutputRate, std::span<const int> Layout,
   uint64_t FormatGeneration)
{
   // Single-chain fixture adapter; production activation always prepares all chains together.
   PreparedEffectChain prepared{std::shared_ptr<AudioEffectChain>(&Chain, [](AudioEffectChain *) {}), 0, {}};
   if (auto error = prepared.prepare(OutputRate, Layout.size() IS 2); error != ERR::Okay) return error;
   std::lock_guard lock(*Chain.Mutex);
   if (prepared.Generation != *Chain.Generation) return ERR::InvalidState;
   prepared.publish(OutputRate, Layout, FormatGeneration);
   return ERR::Okay;
}
#endif

//********************************************************************************************************************

void extAudioEffect::detach()
{
   if (auto chain = Chain.lock()) {
      std::lock_guard mixer_lock(*chain->Mutex);
      std::erase(chain->Effects, this);
      ++*chain->Generation;
   }
   Chain.reset();
}

//********************************************************************************************************************

extAudioEffect::~extAudioEffect()
{
   detach();
}
