// Internal effect-chain operations.  All DSP and chain mutations share the owning mixer mutex.

static void sort_effects(AudioEffectChain &Chain)
{
   std::sort(Chain.Effects.begin(), Chain.Effects.end(), [](auto Left, auto Right) {
      if (Left->Order != Right->Order) return Left->Order < Right->Order;
      return Left->Sequence < Right->Sequence;
   });
}

//********************************************************************************************************************
// Called only with the mixer mutex held.  Hooks must not call Core, allocate, or modify the chain.

void extAudioEffect::process(float *Buffer, int Frames)
{
   if ((Flags & AEF::BYPASS) != AEF::NIL) return;
   if (!processor) return;
   if (ResetPending) {
      processor->reset();
      ResetPending = false;
   }
   processor->process(Buffer, Frames);
}

//********************************************************************************************************************

ERR extAudioEffect::set_processor(std::unique_ptr<AudioEffectProcessor> Processor)
{
   auto chain = Chain.lock();
   if (!chain) return ERR::NotInitialised;
   std::lock_guard mixer_lock(*chain->Mutex);
   if (processor) return ERR::InvalidState;
   processor = std::move(Processor);
   ResetPending = true;
   return ERR::Okay;
}

//********************************************************************************************************************

static void process_effects(AudioEffectChain &Chain, float *Buffer, int Frames)
{
   for (auto effect : Chain.Effects) effect->process(Buffer, Frames);
}

//********************************************************************************************************************

static void configure_effects(AudioEffectChain &Chain, int OutputRate, bool Stereo)
{
   for (auto effect : Chain.Effects) {
      effect->OutputRate = OutputRate;
      effect->Stereo = Stereo;
      effect->ResetPending = true;
   }
}

//********************************************************************************************************************

void extAudioEffect::detach()
{
   if (auto chain = Chain.lock()) {
      std::lock_guard mixer_lock(*chain->Mutex);
      std::erase(chain->Effects, this);
   }
   Chain.reset();
}

//********************************************************************************************************************

extAudioEffect::~extAudioEffect()
{
   detach();
}

