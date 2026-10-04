/*********************************************************************************************************************

-CLASS-
AudioFlanger: A flanger that sweeps a series of comb-filter notches through the sound with a short modulated delay.

The flanger mixes its input with a copy delayed by a few milliseconds.  Mixing the two cancels a series of evenly
spaced frequencies, and a slow oscillator moves the delay so that these notches sweep up and down the spectrum,
producing the characteristic jet-plane whoosh.  Feeding part of the delayed signal back into the delay line deepens
the effect into a pronounced metallic resonance.

Create new flanger objects as a child of an @Audio object or set the inherited #AudioEffect.Audio field.  Use
#AudioEffect.Channel to process one channel set, or leave it at zero to process the global mix.  Application flangers
accept live changes; global flangers become immutable after initialisation.

Parameters are published through the inherited @AudioEffect schema and are changed with @AudioEffect.SetParameter()
followed by @AudioEffect.Flush():

<list type="bullet">
<li>`rate`: the frequency of the sweep, from 0.02 to 10 Hz.  Slow rates give a gradual sweep and fast rates a
vibrating, warbling tone.</li>
<li>`delay`: the shortest delay reached by the sweep, from 0.1 to 5 milliseconds.  Shorter delays place the notches
further apart and higher in the spectrum.</li>
<li>`depth`: how far the delay sweeps above its minimum, from 0 to 10 milliseconds.  The delay moves between `delay`
and `delay + depth`.</li>
<li>`feedback`: the signed proportion of the delayed signal fed back into the delay line, from -90 to 90 percent.
Higher magnitudes give sharper, more resonant peaks between the notches.  Negative values move the peaks and notches
to give a hollower tone.</li>
<li>`mix`: the linear blend of dry and wet signals.  Zero passes the input unchanged, 50 percent gives the deepest
notches and 100 percent outputs only the delayed signal.</li>
</list>

The following adds a slow, resonant sweep to a playing channel set, as a single change:

<pre>
flanger = obj.new('AudioFlanger', { audio=audio, channel=channel })
flanger.mtSetParameter('rate', 0.1)
flanger.mtSetParameter('depth', 4)
flanger.mtSetParameter('feedback', 70)
flanger.acFlush()
</pre>

Other useful combinations include a subtle sweep with `rate` 0.15, `depth` 1.5, `feedback` 0 and `mix` 35; a hollow
tone with `feedback` -60; and a fast vibrato-like warble with `rate` 6, `depth` 0.5 and `mix` 100.

<header>Modulation</header>

Every channel has its own delay line, which is read at `delay + depth * (1 - cos(phase)) / 2` milliseconds.  Both
channels follow the same oscillator, so the stereo image is preserved and no audio passes between the channels.  A
mono output is processed as a single channel.

The oscillator phase is zero whenever the flanger is reset, so the sweep starts at the minimum delay.  It advances
once per output frame while audio is being processed and does not advance while the chain is idle or bypassed.  The
swept delay always stays between 0.1 and 15 milliseconds.  The delay cannot be shorter than one output frame,
because the feedback path must read a sample that has already been written.  At output rates below 10 kHz, delays
shorter than one frame are therefore lengthened to one frame.

The delay line is read by linear interpolation between neighbouring samples.  Interpolation never boosts the signal,
but it softens high frequencies in the delayed copy, by a varying amount as the delay moves.  At zero depth the
delayed copy is a fixed fractional delay and the rate has no effect.  There is no through-zero mode: the delay never
passes through zero, so the notches never sweep to infinite frequency.

<header>Levels and Headroom</header>

Levels are measured in dBFS, where a sample magnitude of 1.0 is 0 dBFS, independently of the output format.
Feedback raises the level of the delayed signal, most strongly at the resonant peaks between the notches.  At the 90
percent limit, the delayed signal of broadband material is about 7 dB louder than the input, and at the resonant
frequencies it can reach ten times (20 dB above) the input level.  The
flanger is stable for every supported setting, including while parameters change, but material with strong tonal
content can exceed 0 dBFS at high feedback.  Lower the input level or follow the flanger with a limiter when high
feedback is required.  Non-finite samples are outside the normalised mixing contract and are treated as silence.

<header>Live Changes</header>

Changes to rate, depth, feedback and mix ramp linearly over 10 ms from their current values.  The rate ramps as a
frequency, so the oscillator continues smoothly from its current phase.  Changes to depth are heard as a brief change
in pitch movement while the ramp moves the delay.

A change to the minimum delay crossfades between two read heads over 10 ms rather than sweeping the delay, so it does
not produce an extra pitch bend.  Both heads follow the same oscillator.  An edit that arrives during the crossfade
replaces a single queued delay, which is applied when the current crossfade finishes.  Flush preserves the delayed
audio and the oscillator phase.  Setting the mix to zero leaves the flanger running, so raising it again reveals the
delayed audio that is present.

Device reactivation, rate or layout changes and leaving bypass discard the delayed audio, return the oscillator phase
to zero and apply the latest parameters without a transition.

<header>Tail and Drain</header>

The flanger has zero algorithmic latency, even at 100 percent mix: the delay and its modulation are the effect itself
and are not compensated.  After its input stops, recirculating audio decays by the feedback proportion on every pass
through the delay line.  The flanger remains pending until the stored audio has fallen 120 dB below the loudest
input and has then left the delay line, and then becomes idle.  At zero feedback this takes 15 ms plus two frames;
at 90 percent feedback and the longest delays it can take a few seconds.  A running oscillator does not keep the
flanger pending.  The decay estimate used when other audio shares the flanger covers a 60 dB decay at the longest
delay that the current settings can reach.  Hard bypass and destruction cut the tail abruptly.

Output rates from 8000 to 192000 Hz are supported.  Configuring the effect for any other rate fails with
`ERR::NoSupport`.  The delay memory covers 15 ms and requires approximately 6 KB at 48 kHz in stereo, scaling in
proportion to the output rate.

-END-

*********************************************************************************************************************/

#include "audio_flanger_dsp.h"

class extAudioFlanger : public extAudioEffect {
public:
   FlangerSettings Settings;
   FlangerProcessor *Processor = nullptr;

   extAudioFlanger(objMetaClass *ClassPtr, OBJECTID ObjectID);
};

//********************************************************************************************************************
// Parameter descriptors.  Order matches the FL_ indexes.

enum { FL_RATE = 0, FL_DELAY, FL_DEPTH, FL_FEEDBACK, FL_MIX };

static const AudioParamDesc glFlangerParams[] = {
   { .Key = "rate", .Label = "Rate",
     .Description = "The frequency of the delay sweep.",
     .Unit = APU::HZ, .Scale = APS::LOG, .Min = 0.02, .Max = 10, .Default = 0.25 },
   { .Key = "delay", .Label = "Delay",
     .Description = "The shortest delay reached by the sweep.",
     .Unit = APU::MS, .Scale = APS::LOG, .Min = 0.1, .Max = 5, .Default = 1 },
   { .Key = "depth", .Label = "Depth",
     .Description = "How far the delay sweeps above its minimum.  Zero disables the modulation.",
     .Unit = APU::MS, .Min = 0, .Max = 10, .Default = 2 },
   { .Key = "feedback", .Label = "Feedback",
     .Description = "The signed proportion of the delayed signal fed back into the delay.  Higher magnitudes give "
        "stronger resonance; negative values give a hollower tone.",
     .Unit = APU::PERCENT, .Min = -90, .Max = 90, .Default = 30 },
   { .Key = "mix", .Label = "Mix",
     .Description = "The proportion of the delayed signal in the output.  Zero leaves the input unchanged and 50% "
        "gives the deepest notches.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 100, .Default = 50 }
};

static const AudioOutputDesc glFlangerOutputs[] = {
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", "Maximum input sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", "Maximum output sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::OUTPUT_PEAK }
};

//********************************************************************************************************************

static FlangerSettings flanger_settings(const AudioParamState &State)
{
   return FlangerSettings {
      .Rate     = State.Params[FL_RATE],
      .Delay    = State.Params[FL_DELAY],
      .Depth    = State.Params[FL_DEPTH],
      .Feedback = State.Params[FL_FEEDBACK],
      .Mix      = State.Params[FL_MIX]
   };
}

static void flanger_read(extAudioEffect *Effect, AudioParamState &State)
{
   const auto &settings = ((extAudioFlanger *)Effect)->Settings;
   State.Params.assign({ settings.Rate, settings.Delay, settings.Depth, settings.Feedback, settings.Mix });
   State.Groups.clear();
}

//********************************************************************************************************************
// Before initialisation there is no processor.

static void flanger_apply(extAudioEffect *Effect, const AudioParamState &State)
{
   ((extAudioFlanger *)Effect)->Settings = flanger_settings(State);
}

//********************************************************************************************************************
// Targets are derived outside the mixer lock.  An unsupported rate leaves the target invalid, in which case only the
// settings are published and the processor remains inactive.

class FlangerUpdate final : public AudioParamUpdate {
public:
   FlangerSettings Settings;
   FlangerTarget Target;
   bool Valid;

   FlangerUpdate(const AudioParamState &State, int Rate) : Settings(flanger_settings(State)) {
      Valid = flanger_target(Settings, Rate, Target);
   }

   void publish(extAudioEffect *Effect) override {
      auto Self = (extAudioFlanger *)Effect;
      if (Self->Processor) Self->Processor->update(Settings, Valid ? &Target : nullptr);
      Self->Settings = Settings;
   }
};

static std::unique_ptr<AudioParamUpdate> flanger_prepare(extAudioEffect *Effect, const AudioParamState &State,
   int Rate)
{
   return std::make_unique<FlangerUpdate>(State, Rate);
}

//********************************************************************************************************************

static const AudioEffectSchema glFlangerSchema = {
   .ClassName   = "AudioFlanger",
   .Version     = 1,
   .Description = "A flanger that sweeps a series of comb-filter notches through the sound with a short modulated "
      "delay.",
   .Params      = glFlangerParams,
   .Outputs     = glFlangerOutputs,
   .Read        = flanger_read,
   .Apply       = flanger_apply,
   .Prepare     = flanger_prepare
};

//********************************************************************************************************************

extAudioFlanger::extAudioFlanger(objMetaClass *ClassPtr, OBJECTID ObjectID) : extAudioEffect(ClassPtr, ObjectID)
{
   Schema = &glFlangerSchema;
}

//********************************************************************************************************************
// Storage is allocated by set_processor() outside the mixer lock.

static ERR AUDIOFLANGER_Init(extAudioFlanger *Self)
{
   if (Self->Processor) return ERR::InvalidState;
   auto chain = Self->Chain.lock();
   if (not chain) return ERR::NotInitialised;

   AudioParamState state;
   flanger_read(Self, state);
   {
      std::lock_guard mixer_lock(*chain->Mutex);
      if (validate_state(glFlangerSchema, state, Self->OutputRate) != ERR::Okay) return ERR::InvalidValue;
   }

   auto processor = std::make_unique<FlangerProcessor>(Self, Self->Settings);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;

   return error;
}

//********************************************************************************************************************

#include "class_audioflanger_def.c"

//********************************************************************************************************************

static ERR add_audioflanger_class()
{
   clAudioFlanger = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIOEFFECT),
      fl::ClassID(CLASSID::AUDIOFLANGER),
      fl::Name("AudioFlanger"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioFlangerActions),
      fl::Size(sizeof(extAudioFlanger)),
      fl::Path(MOD_PATH));
   return clAudioFlanger ? ERR::Okay : ERR::AddClass;
}
