/*********************************************************************************************************************

-CLASS-
AudioReverb: An algorithmic reverberator that simulates room ambience.

The reverberator is a feedback-delay network of eight delay lines mixed by an orthogonal matrix, with an optional
early-reflection stage that simulates the first distinct echoes from the walls of the room.

Create new reverberator objects as a child of an @Audio object or set the inherited #AudioEffect.Audio field.
Use #AudioEffect.Channel to process one channel set, or leave it at zero to process the global mix.  Application
reverberators accept live changes; global reverberators become immutable after initialisation.

Each parameter is available as a field, such as #Decay and #Size.  Field writes before initialisation are range-checked
and stored; after initialisation each write is validated and applied immediately.  The same parameters are published
through the inherited @AudioEffect schema under lower-case keys, where they can be changed together with
@AudioEffect.Batch():

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
<li>`early_level`: the level of the early reflections in the wet output, from 0 to 100 percent.  The default of zero
disables them.</li>
<li>`early_length`: the time from the first to the last early reflection, from 5 to 100 ms, which corresponds to the
length of the room.</li>
<li>`mix`: the linear blend of dry and wet signals.  Zero passes the input unchanged and 100 percent outputs only
the reverberation.</li>
</list>

The wet signal is normalised so that its energy is close to that of the input, whatever the decay and size.  Longer
decays therefore spread the same energy over a longer time rather than becoming louder.  Damping removes energy and is
not compensated.  The normalisation holds on average across frequencies, but the reverberation's response is uneven,
so a sustained tone can emerge up to about 10 dB above its input level.  Leave headroom at high mix settings, or place
an effect that limits peaks after the reverb.

The following creates a large hall for a playing channel set, then lengthens its decay:

<pre>
reverb = obj.new('AudioReverb', { audio=audio, channel=channel, size=90, mix=30 })
reverb.decay = 3500
</pre>

Every field write after initialisation is a separate change.  To change several parameters in a single step, apply them
together with @AudioEffect.Batch():

<pre>
reverb.mtBatch(array&lt;str&gt; { 'set size 40', 'set decay 900', 'set early_level 50' })
</pre>

A mono output is processed as a single channel.  A stereo input feeds alternate delay lines from each channel, and the
two outputs are taken from orthogonal combinations of every line, so the wet signal is decorrelated between the
speakers.

<header>Early Reflections</header>

The early reflections are eight taps on a delay line that each channel feeds after the pre-delay.  Their times follow
a fixed pattern of prime numbers that is scaled by `early_length`, and the left and right channels use different
times so that the reflections are decorrelated between the speakers.  The first left reflection arrives at the end of
the pre-delay and the last right reflection arrives `early_length` later.  A mono output uses the left pattern.  The
amplitude of each reflection falls with the distance it travels, and the reflections of an impulse carry the same
energy as the input, as the late reverberation does.

The early reflections are added to the wet output and do not feed the network, so the late reverberation is the same
at every `early_level`.  At zero, the output is identical to that of a reverberator without early reflections.  Short
lengths at high levels can sound like a flutter echo; combine them with a moderate `early_level`.

The following adds the reflections of a long room to a small, diffuse reverberation:

<pre>
reverb.mtBatch(array&lt;str&gt; { 'set early_level 60', 'set early_length 70', 'set size 30' })
</pre>

<header>Live Changes</header>

Changes to decay, damping, diffusion, early level and mix ramp linearly over 10 ms and preserve the reverberation that
is already present.  A pre-delay or early length change crossfades from the old to the new delays over 10 ms, avoiding
a pitch sweep.  A size
change crossfades the input to a second network over 10 ms, while the output of the previous network fades out over
100 ms.
Both networks are allocated when the processor is configured, so edits never allocate memory, and processing cost is
up to twice the normal level during a size transition.  An edit that arrives during a pre-delay or early length
crossfade, or a size transition, replaces a single queued target, which is applied when the current transition
finishes.

Device reactivation, leaving bypass and reaching idle discard the reverberation and apply the latest parameters
without a transition.

<header>Tail and Drain</header>

The reverb has zero algorithmic latency.  After its input stops, it remains pending until every value written to its
delay memory has stayed at least 120 dB below the largest input sample since the last reset, for long enough to
overwrite that memory.  This takes approximately 1.5 times the decay time after a short sound, and up to 2.5 times
after a sustained tone, which builds up a higher internal level.  Add the pre-delay and 250 ms to both figures.  The
residual is relative to the input, so it is independent of the output bit depth.

When other audio shares the reverb, a stopped voice's tail is estimated as the pre-delay, the longest delay line, the
early reflection length when the early reflections are audible, and the decay time.  The published tail bound covers
the early reflection length and 180 dB of decay.  A 60 dB decay time is not a
guarantee of complete silence: with long decay settings, draining can reach @Audio.MaxDrain, which then fades and
discards the remainder.  Hard bypass and destruction cut the tail abruptly.

Output rates from 8000 to 192000 Hz are supported.  Configuring the effect for any other rate fails with
`ERR::NoSupport`.  Memory use is approximately 340 KB at 48 kHz in stereo, and scales in proportion to the output
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

enum { RV_DECAY = 0, RV_SIZE, RV_PRE_DELAY, RV_DAMPING, RV_DIFFUSION, RV_EARLY_LEVEL, RV_EARLY_LENGTH, RV_MIX };

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
   { .Key = "early_level", .Label = "Early Level",
     .Description = "The level of the early reflections in the wet output.  Zero disables them.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 100, .Default = 0 },
   { .Key = "early_length", .Label = "Early Length",
     .Description = "The time from the first to the last early reflection, i.e. the length of the room.",
     .Unit = APU::MS, .Scale = APS::LOG, .Min = 5, .Max = 100, .Default = 30 },
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
      .Decay       = State.Params[RV_DECAY],
      .Size        = State.Params[RV_SIZE],
      .PreDelay    = State.Params[RV_PRE_DELAY],
      .Damping     = State.Params[RV_DAMPING],
      .Diffusion   = State.Params[RV_DIFFUSION],
      .EarlyLevel  = State.Params[RV_EARLY_LEVEL],
      .EarlyLength = State.Params[RV_EARLY_LENGTH],
      .Mix         = State.Params[RV_MIX]
   };
}

static void reverb_read(extAudioEffect *Effect, AudioParamState &State)
{
   const auto &settings = ((extAudioReverb *)Effect)->Settings;
   State.Params.assign({ settings.Decay, settings.Size, settings.PreDelay, settings.Damping, settings.Diffusion,
      settings.EarlyLevel, settings.EarlyLength, settings.Mix });
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
   .Version     = 2,
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

/*********************************************************************************************************************

-FIELD-
Damping: How much faster high frequencies decay than low frequencies, as a percentage.

The range is 0 to 100% and the default is 50%.  This field mirrors the `damping` schema key.  To change it together with
other parameters in a single step, use @AudioEffect.Batch().

-FIELD-
Decay: The time, in milliseconds, for the reverberation to fall by 60 dB at low frequencies.

The range is 100 to 20000 ms and the default is 1500 ms.  This field mirrors the `decay` schema key.  To change it
together with other parameters in a single step, use @AudioEffect.Batch().

-FIELD-
Diffusion: The density of the reflections, as a percentage.

The range is 0 to 100% and the default is 70%.  Low values produce distinct echoes and high values a smooth wash.  This
field mirrors the `diffusion` schema key.  To change it together with other parameters in a single step, use
@AudioEffect.Batch().

-FIELD-
EarlyLength: The time, in milliseconds, from the first to the last early reflection.

The range is 5 to 100 ms and the default is 30 ms.  This field mirrors the `early_length` schema key.  To change it
together with other parameters in a single step, use @AudioEffect.Batch().

-FIELD-
EarlyLevel: The level of the early reflections in the wet output, as a percentage.

The range is 0 to 100% and the default is 0%.  Zero disables the early reflections.  This field mirrors the
`early_level` schema key.  To change it together with other parameters in a single step, use @AudioEffect.Batch().

-FIELD-
Mix: The proportion of reverberation in the output, as a percentage.

The range is 0 to 100% and the default is 20%.  Zero leaves the input unchanged.  This field mirrors the `mix` schema
key.  To change it together with other parameters in a single step, use @AudioEffect.Batch().

-FIELD-
PreDelay: The gap, in milliseconds, before the input reaches the reverberation.

The range is 0 to 250 ms and the default is 20 ms.  This field mirrors the `pre_delay` schema key.  To change it
together with other parameters in a single step, use @AudioEffect.Batch().

-FIELD-
Size: The scale of the simulated room, as a percentage.

The range is 0 to 100% and the default is 50%.  Larger rooms space their reflections further apart.  This field mirrors
the `size` schema key.  To change it together with other parameters in a single step, use @AudioEffect.Batch().

*********************************************************************************************************************/

#include "class_audioreverb_def.c"

static const FieldArray clAudioReverbFields[] = {
   { "Decay",      FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE,
     effect_settings_get<extAudioReverb, &ReverbSettings::Decay>, effect_param_set<RV_DECAY> },
   { "Size",       FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE,
     effect_settings_get<extAudioReverb, &ReverbSettings::Size>, effect_param_set<RV_SIZE> },
   { "PreDelay",   FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE,
     effect_settings_get<extAudioReverb, &ReverbSettings::PreDelay>, effect_param_set<RV_PRE_DELAY> },
   { "Damping",    FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE,
     effect_settings_get<extAudioReverb, &ReverbSettings::Damping>, effect_param_set<RV_DAMPING> },
   { "Diffusion",  FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE,
     effect_settings_get<extAudioReverb, &ReverbSettings::Diffusion>, effect_param_set<RV_DIFFUSION> },
   { "EarlyLevel", FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE,
     effect_settings_get<extAudioReverb, &ReverbSettings::EarlyLevel>, effect_param_set<RV_EARLY_LEVEL> },
   { "EarlyLength", FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE,
     effect_settings_get<extAudioReverb, &ReverbSettings::EarlyLength>, effect_param_set<RV_EARLY_LENGTH> },
   { "Mix",        FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE,
     effect_settings_get<extAudioReverb, &ReverbSettings::Mix>, effect_param_set<RV_MIX> },
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
