/*********************************************************************************************************************

-CLASS-
AudioReverb: An algorithmic reverberator that simulates room ambience.

The reverberator is a feedback-delay network of eight delay lines mixed by an orthogonal matrix.

Create new reverberator objects as a child of an @Audio object or set the inherited #AudioEffect.Audio field.
Use #AudioEffect.Channel to process one channel set, or leave it at zero to process the global mix.  Application
reverberators accept live changes; global reverberators become immutable after initialisation.

Parameters are published through the inherited @AudioEffect schema and are changed with
@AudioEffect.SetParameter() followed by @AudioEffect.Flush():

<list type="bullet">
<li>`decay`: the time, in milliseconds, for the reverberation to fall by 60 dB at low frequencies.</li>
<li>`size`: the scale of the simulated room from 0 to 100 percent.  Delay-line lengths range from approximately
9-18 ms at zero to 43-90 ms at 100 percent.</li>
<li>`pre_delay`: a gap of up to 250 ms that is inserted before the input reaches the reverberator.</li>
<li>`damping`: how much faster high frequencies decay than low frequencies.  The decay time at 4 kHz is reduced by
0.9% for every percent of damping, so 100 percent decays 4 kHz ten times faster than low frequencies.  At output
rates below 10 kHz the reference frequency is 40% of the output rate.</li>
<li>`diffusion`: the density of the reflections, controlled by four all-pass diffusers on each input channel.  Their
coefficient is 0.75 at 100 percent.</li>
<li>`mix`: the linear blend of dry and wet signals.  Zero passes the input unchanged and 100 percent outputs only
the reverberation.</li>
</list>

The wet signal is normalised so that its energy is close to that of the input, whatever the decay and size.  Longer
decays therefore spread the same energy over a longer time rather than becoming louder.  Damping removes energy and is
not compensated.  The normalisation holds on average across frequencies, but the reverberation's response is uneven,
so a sustained tone can emerge up to about 10 dB above its input level.  Leave headroom at high mix settings, or place
an effect that limits peaks after the reverb.

The following creates a large hall for a playing channel set, then lengthens its decay as a single change:

<pre>
reverb = obj.new('AudioReverb', { audio=audio, channel=channel })
reverb.mtSetParameter('size', 90)
reverb.mtSetParameter('decay', 3500)
reverb.mtSetParameter('mix', 30)
reverb.acFlush()
</pre>

A mono output is processed as a single channel.  A stereo input feeds alternate delay lines from each channel, and the
two outputs are taken from orthogonal combinations of every line, so the wet signal is decorrelated between the
speakers.

<header>Live Changes</header>

Changes to decay, damping, diffusion and mix ramp linearly over 10 ms and preserve the reverberation that is already
present.  A pre-delay change crossfades from the old to the new delay over 10 ms, avoiding a pitch sweep.  A size
change crossfades the input to a second network over 10 ms, while the output of the previous network fades out over
100 ms.
Both networks are allocated when the processor is configured, so edits never allocate memory, and processing cost is
up to twice the normal level during a size transition.  An edit that arrives during a pre-delay crossfade or a size
transition replaces a single queued target, which is applied when the current transition finishes.

Device reactivation, leaving bypass and reaching idle discard the reverberation and apply the latest parameters
without a transition.

<header>Tail and Drain</header>

The reverb has zero algorithmic latency.  After its input stops, it remains pending until every value written to its
delay memory has stayed at least 120 dB below the largest input sample since the last reset, for long enough to
overwrite that memory.  This takes approximately 1.5 times the decay time after a short sound, and up to 2.5 times
after a sustained tone, which builds up a higher internal level.  Add the pre-delay and 250 ms to both figures.  The
residual is relative to the input, so it is independent of the output bit depth.

When other audio shares the reverb, a stopped voice's tail is estimated as the pre-delay, the longest delay line and
the decay time.  The published tail bound covers 180 dB of decay.  A 60 dB decay time is not a
guarantee of complete silence: with long decay settings, draining can reach @Audio.MaxDrain, which then fades and
discards the remainder.  Hard bypass and destruction cut the tail abruptly.

Output rates from 8000 to 192000 Hz are supported.  Configuring the effect for any other rate fails with
`ERR::NoSupport`.  Memory use is approximately 300 KB at 48 kHz in stereo, and scales in proportion to the output
rate.

-END-

*********************************************************************************************************************/

#include "audio_reverb_dsp.h"

class extAudioReverb : public extAudioEffect {
public:
   ReverbSettings Settings;
   ReverbProcessor *Processor = nullptr;

   extAudioReverb(objMetaClass *ClassPtr, OBJECTID ObjectID);
};

//********************************************************************************************************************
// Parameter descriptors.  Order matches the RV_ indexes.

enum { RV_DECAY = 0, RV_SIZE, RV_PRE_DELAY, RV_DAMPING, RV_DIFFUSION, RV_MIX };

static const AudioParamDesc glReverbParams[] = {
   { .Key = "decay", .Label = "Decay",
     .Description = "The time for the reverberation to fall by 60 dB at low frequencies.",
     .Unit = APU::MS, .Scale = APS::LOG, .Min = 100, .Max = 20000, .Default = 1500 },
   { .Key = "size", .Label = "Size",
     .Description = "The scale of the simulated room.  Larger rooms space their reflections further apart.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 100, .Default = 50 },
   { .Key = "pre_delay", .Label = "Pre-Delay",
     .Description = "The gap before the input reaches the reverberation, separating it from the dry sound.",
     .Unit = APU::MS, .Min = 0, .Max = 250, .Default = 20 },
   { .Key = "damping", .Label = "Damping",
     .Description = "How much faster high frequencies decay than low frequencies, simulating absorbent surfaces.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 100, .Default = 50 },
   { .Key = "diffusion", .Label = "Diffusion",
     .Description = "The density of the reflections.  Low values produce distinct echoes and high values a smooth "
        "wash.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 100, .Default = 70 },
   { .Key = "mix", .Label = "Mix",
     .Description = "The proportion of reverberation in the output.  Zero leaves the input unchanged.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 100, .Default = 20 }
};

static const AudioOutputDesc glReverbOutputs[] = {
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", "Maximum input sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", "Maximum output sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::OUTPUT_PEAK }
};

//********************************************************************************************************************

static ReverbSettings state_settings(const AudioParamState &State)
{
   return ReverbSettings {
      .Decay     = State.Params[RV_DECAY],
      .Size      = State.Params[RV_SIZE],
      .PreDelay  = State.Params[RV_PRE_DELAY],
      .Damping   = State.Params[RV_DAMPING],
      .Diffusion = State.Params[RV_DIFFUSION],
      .Mix       = State.Params[RV_MIX]
   };
}

static void reverb_read(extAudioEffect *Effect, AudioParamState &State)
{
   const auto &settings = ((extAudioReverb *)Effect)->Settings;
   State.Params.assign({ settings.Decay, settings.Size, settings.PreDelay, settings.Damping, settings.Diffusion,
      settings.Mix });
   State.Groups.clear();
}

//********************************************************************************************************************
// Before initialisation there is no processor.

static void reverb_apply(extAudioEffect *Effect, const AudioParamState &State)
{
   ((extAudioReverb *)Effect)->Settings = state_settings(State);
}

//********************************************************************************************************************
// Coefficients are derived outside the mixer lock.  An unsupported rate leaves the target invalid, in which case
// only the settings are published and the processor remains inactive.

class ReverbUpdate final : public AudioParamUpdate {
public:
   ReverbSettings Settings;
   ReverbTarget Target;
   bool Valid;

   ReverbUpdate(const AudioParamState &State, int Rate) : Settings(state_settings(State)) {
      Valid = reverb_target(Settings, Rate, Target);
   }

   void publish(extAudioEffect *Effect) override {
      auto Self = (extAudioReverb *)Effect;
      if (Self->Processor) Self->Processor->update(Settings, Valid ? &Target : nullptr);
      Self->Settings = Settings;
   }
};

static std::unique_ptr<AudioParamUpdate> reverb_prepare(extAudioEffect *Effect, const AudioParamState &State, int Rate)
{
   return std::make_unique<ReverbUpdate>(State, Rate);
}

//********************************************************************************************************************

static const AudioEffectSchema glReverbSchema = {
   .ClassName   = "AudioReverb",
   .Version     = 1,
   .Description = "An algorithmic reverberator that simulates room ambience.",
   .Params      = glReverbParams,
   .Outputs     = glReverbOutputs,
   .Read        = reverb_read,
   .Apply       = reverb_apply,
   .Prepare     = reverb_prepare
};

//********************************************************************************************************************

extAudioReverb::extAudioReverb(objMetaClass *ClassPtr, OBJECTID ObjectID) : extAudioEffect(ClassPtr, ObjectID)
{
   Schema = &glReverbSchema;
}

//********************************************************************************************************************
// Storage is allocated by set_processor() outside the mixer lock.

static ERR AUDIOREVERB_Init(extAudioReverb *Self)
{
   if (Self->Processor) return ERR::InvalidState;
   auto chain = Self->Chain.lock();
   if (not chain) return ERR::NotInitialised;

   AudioParamState state;
   reverb_read(Self, state);
   {
      std::lock_guard mixer_lock(*chain->Mutex);
      if (validate_state(glReverbSchema, state, Self->OutputRate) != ERR::Okay) return ERR::InvalidValue;
   }

   auto processor = std::make_unique<ReverbProcessor>(Self, Self->Settings);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;

   return error;
}

//********************************************************************************************************************

#include "class_audioreverb_def.c"

static const FieldArray clAudioReverbFields[] = {
   END_FIELD
};

//********************************************************************************************************************

static ERR add_audioreverb_class()
{
   clAudioReverb = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIOEFFECT),
      fl::ClassID(CLASSID::AUDIOREVERB),
      fl::Name("AudioReverb"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioReverbActions),
      fl::Fields(clAudioReverbFields),
      fl::Size(sizeof(extAudioReverb)),
      fl::Path(MOD_PATH));
   return clAudioReverb ? ERR::Okay : ERR::AddClass;
}
