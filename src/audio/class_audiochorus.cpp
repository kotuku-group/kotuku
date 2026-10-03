/*********************************************************************************************************************

-CLASS-
AudioChorus: A chorus that thickens the sound by mixing in a copy whose delay is swept by a slow oscillator.

The chorus delays its input by a short interval that a sine oscillator moves back and forth.  The moving delay
shifts the pitch of the delayed copy slightly up and down, and mixing that copy with the original produces the
shimmering, widened sound of several performers playing together.

Create new chorus objects as a child of an @Audio object or set the inherited #AudioEffect.Audio field.  Use
#AudioEffect.Channel to process one channel set, or leave it at zero to process the global mix.  Application choruses
accept live changes; global choruses become immutable after initialisation.

Parameters are published through the inherited @AudioEffect schema and are changed with @AudioEffect.SetParameter()
followed by @AudioEffect.Flush():

<list type="bullet">
<li>`rate`: the frequency of the sweep, from 0.05 to 10 Hz.  Slow rates give a gentle drift and fast rates a warble.
</li>
<li>`delay`: the centre of the sweep, from 10 to 30 milliseconds.</li>
<li>`depth`: how far the delay moves either side of its centre, from 0 to 9 milliseconds.  Greater depths and faster
rates give a stronger pitch variation.</li>
<li>`spread`: the phase difference between the left and right sweeps, from 0 to 100 percent of half a cycle.  Zero
moves both channels together and 100 percent moves them in opposition for the widest stereo image.</li>
<li>`mix`: the linear blend of dry and wet signals.  Zero passes the input unchanged and 100 percent outputs only the
delayed copy.</li>
</list>

The following adds a wide, slow chorus to a playing channel set, as a single change:

<pre>
chorus = obj.new('AudioChorus', { audio=audio, channel=channel })
chorus.mtSetParameter('rate', 0.4)
chorus.mtSetParameter('depth', 5)
chorus.mtSetParameter('spread', 100)
chorus.acFlush()
</pre>

<header>Modulation</header>

Every channel has its own delay line, which is read at `delay + depth * sin(phase + offset)` milliseconds.  The
offset is zero on the left and `180 * spread / 100` degrees on the right, so the stereo image is preserved and no audio
passes between the channels.  A mono output is processed as a single channel with an offset of zero; the `spread`
value is retained and applies if the output becomes stereo.

The oscillator phase is zero whenever the chorus is reset, and advances once per output frame while audio is being
processed.  It does not advance while the chain is idle or bypassed.  The swept delay always stays between 1 and 39
milliseconds.

The delay line is read by linear interpolation between neighbouring samples.  Interpolation never boosts the signal,
but it softens high frequencies in the delayed copy, by a varying amount as the delay moves, and adds low-level
modulation artefacts.  These are most noticeable on bright material with fully wet settings.  At zero depth the
delayed copy is a fixed fractional delay and the rate and spread have no effect.

The chorus has a single delayed voice per channel and no feedback, so the delayed copy never repeats and the effect
cannot sustain itself after its input stops.

<header>Levels and Headroom</header>

Levels are measured in dBFS, where a sample magnitude of 1.0 is 0 dBFS, independently of the output format.  The
output is a weighted average of the input and a delayed copy of it, so no output sample exceeds the largest input
magnitude of the last 39 ms.  Input above unity passes through without limiting or clipping.  Non-finite samples
are outside the normalised mixing contract and are treated as silence.

<header>Live Changes</header>

Changes to rate, depth, spread and mix ramp linearly over 10 ms from their current values.  The rate ramps as a
frequency, so the oscillator continues smoothly from its current phase.  These are deliberate changes to the
modulation and are heard as changes in pitch movement.

A change to the centre delay crossfades between two read heads over 10 ms rather than sweeping the delay, so it does
not produce an extra pitch bend.  Both heads follow the same oscillator.  An edit that arrives during the crossfade
replaces a single queued delay, which is applied when the current crossfade finishes.  Flush preserves the delayed
audio and the oscillator phase.  Setting the mix to zero leaves the chorus running, so raising it again reveals the
delayed audio that is present.

Device reactivation, rate or layout changes and leaving bypass discard the delayed audio, return the oscillator phase
to zero and apply the latest parameters without a transition.

<header>Tail and Drain</header>

The chorus has zero algorithmic latency, even at 100 percent mix: the delay and its modulation are the effect itself
and are not compensated.  After its input stops, the chorus remains pending for 39 ms plus two frames, which covers
every sample that any setting can read, and then becomes idle.  A running oscillator does not keep the chorus
pending.  The same interval is used as the decay estimate when other audio shares the chorus.  Hard bypass and
destruction cut the tail abruptly.

Output rates from 8000 to 192000 Hz are supported.  Configuring the effect for any other rate fails with
`ERR::NoSupport`.  The delay memory covers 39 ms and requires approximately 15 KB at 48 kHz in stereo, scaling in
proportion to the output rate.

-END-

*********************************************************************************************************************/

#include "audio_chorus_dsp.h"

class extAudioChorus : public extAudioEffect {
public:
   ChorusSettings Settings;
   ChorusProcessor *Processor = nullptr;

   extAudioChorus(objMetaClass *ClassPtr, OBJECTID ObjectID);
};

//********************************************************************************************************************
// Parameter descriptors.  Order matches the CH_ indexes.

enum { CH_RATE = 0, CH_DELAY, CH_DEPTH, CH_SPREAD, CH_MIX };

static const AudioParamDesc glChorusParams[] = {
   { .Key = "rate", .Label = "Rate",
     .Description = "The frequency of the delay sweep.",
     .Unit = APU::HZ, .Scale = APS::LOG, .Min = 0.05, .Max = 10, .Default = 0.8 },
   { .Key = "delay", .Label = "Delay",
     .Description = "The centre of the delay sweep.",
     .Unit = APU::MS, .Min = 10, .Max = 30, .Default = 15 },
   { .Key = "depth", .Label = "Depth",
     .Description = "How far the delay moves either side of its centre.  Zero disables the modulation.",
     .Unit = APU::MS, .Min = 0, .Max = 9, .Default = 3 },
   { .Key = "spread", .Label = "Spread",
     .Description = "The phase difference between the left and right sweeps, up to half a cycle.  "
        "Mono output is unaffected.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 100, .Default = 50 },
   { .Key = "mix", .Label = "Mix",
     .Description = "The proportion of the delayed copy in the output.  Zero leaves the input unchanged.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 100, .Default = 35 }
};

static const AudioOutputDesc glChorusOutputs[] = {
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", "Maximum input sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", "Maximum output sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::OUTPUT_PEAK }
};

//********************************************************************************************************************

static ChorusSettings chorus_settings(const AudioParamState &State)
{
   return ChorusSettings {
      .Rate   = State.Params[CH_RATE],
      .Delay  = State.Params[CH_DELAY],
      .Depth  = State.Params[CH_DEPTH],
      .Spread = State.Params[CH_SPREAD],
      .Mix    = State.Params[CH_MIX]
   };
}

static void chorus_read(extAudioEffect *Effect, AudioParamState &State)
{
   const auto &settings = ((extAudioChorus *)Effect)->Settings;
   State.Params.assign({ settings.Rate, settings.Delay, settings.Depth, settings.Spread, settings.Mix });
   State.Groups.clear();
}

//********************************************************************************************************************
// Before initialisation there is no processor.

static void chorus_apply(extAudioEffect *Effect, const AudioParamState &State)
{
   ((extAudioChorus *)Effect)->Settings = chorus_settings(State);
}

//********************************************************************************************************************
// Targets are derived outside the mixer lock.  An unsupported rate leaves the target invalid, in which case only the
// settings are published and the processor remains inactive.

class ChorusUpdate final : public AudioParamUpdate {
public:
   ChorusSettings Settings;
   ChorusTarget Target;
   bool Valid;

   ChorusUpdate(const AudioParamState &State, int Rate) : Settings(chorus_settings(State)) {
      Valid = chorus_target(Settings, Rate, Target);
   }

   void publish(extAudioEffect *Effect) override {
      auto Self = (extAudioChorus *)Effect;
      if (Self->Processor) Self->Processor->update(Settings, Valid ? &Target : nullptr);
      Self->Settings = Settings;
   }
};

static std::unique_ptr<AudioParamUpdate> chorus_prepare(extAudioEffect *Effect, const AudioParamState &State, int Rate)
{
   return std::make_unique<ChorusUpdate>(State, Rate);
}

//********************************************************************************************************************

static const AudioEffectSchema glChorusSchema = {
   .ClassName   = "AudioChorus",
   .Version     = 1,
   .Description = "A chorus that thickens the sound by mixing in a copy whose delay is swept by a slow oscillator.",
   .Params      = glChorusParams,
   .Outputs     = glChorusOutputs,
   .Read        = chorus_read,
   .Apply       = chorus_apply,
   .Prepare     = chorus_prepare
};

//********************************************************************************************************************

extAudioChorus::extAudioChorus(objMetaClass *ClassPtr, OBJECTID ObjectID) : extAudioEffect(ClassPtr, ObjectID)
{
   Schema = &glChorusSchema;
}

//********************************************************************************************************************
// Storage is allocated by set_processor() outside the mixer lock.

static ERR AUDIOCHORUS_Init(extAudioChorus *Self)
{
   if (Self->Processor) return ERR::InvalidState;
   auto chain = Self->Chain.lock();
   if (not chain) return ERR::NotInitialised;

   AudioParamState state;
   chorus_read(Self, state);
   {
      std::lock_guard mixer_lock(*chain->Mutex);
      if (validate_state(glChorusSchema, state, Self->OutputRate) != ERR::Okay) return ERR::InvalidValue;
   }

   auto processor = std::make_unique<ChorusProcessor>(Self, Self->Settings);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;

   return error;
}

//********************************************************************************************************************

#include "class_audiochorus_def.c"

//********************************************************************************************************************

static ERR add_audiochorus_class()
{
   clAudioChorus = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIOEFFECT),
      fl::ClassID(CLASSID::AUDIOCHORUS),
      fl::Name("AudioChorus"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioChorusActions),
      fl::Size(sizeof(extAudioChorus)),
      fl::Path(MOD_PATH));
   return clAudioChorus ? ERR::Okay : ERR::AddClass;
}
