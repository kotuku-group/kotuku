/*********************************************************************************************************************

-CLASS-
AudioPhaser: A phaser that sweeps a set of notches through the sound by mixing it with an all-pass filtered copy.

The phaser passes its input through a cascade of six all-pass filters, which leave every frequency at the same level
but delay each by a different phase.  Mixing the filtered copy with the dry input cancels the frequencies where the
two are out of phase, giving three notches, and a slow oscillator sweeps the filters so that the notches glide up and
down the spectrum.  Unlike a flanger, whose notches are evenly spaced, the phaser's notches are spread unevenly and
fewer in number, giving a softer, swirling sound.  Feeding part of the filtered signal back into the cascade
sharpens the peaks between the notches into a vocal resonance.

Create new phaser objects as a child of an @Audio object or set the inherited #AudioEffect.Audio field.  Use
#AudioEffect.Channel to process one channel set, or leave it at zero to process the global mix.  Application phasers
accept live changes; global phasers become immutable after initialisation.

Parameters are published through the inherited @AudioEffect schema and are changed with @AudioEffect.SetKey()
followed by @AudioEffect.Flush():

<list type="bullet">
<li>`rate`: the frequency of the sweep, from 0.02 to 10 Hz.  Slow rates give a gradual sweep and fast rates a
warbling tone.</li>
<li>`centre`: the frequency at the middle of the sweep, from 100 to 4000 Hz.  Low centres concentrate the effect on
bass and lower midrange, high centres on presence and treble.</li>
<li>`depth`: how far the sweep travels either side of the centre, from 0 to 100 percent of two octaves.  At zero the
notches stand still and the rate has no effect.</li>
<li>`feedback`: the signed proportion of the filtered signal fed back into the cascade, from -80 to 80 percent.
Higher magnitudes give sharper, more resonant peaks.  Negative values move the peaks to frequencies between the
positive ones.</li>
<li>`mix`: the linear blend of dry and filtered signals.  Zero passes the input unchanged, 50 percent gives the
deepest notches and 100 percent outputs only the filtered signal, in which the phase sweep is heard only as a slight
vibrato.</li>
</list>

The following adds a slow, resonant sweep to a playing channel set, as a single change:

<pre>
phaser = obj.new('AudioPhaser', { audio=audio, channel=channel })
phaser.acSetKey('rate', 0.15)
phaser.acSetKey('depth', 90)
phaser.acSetKey('feedback', 60)
phaser.acFlush()
</pre>

Other useful combinations include a subtle swirl with `rate` 0.3, `depth` 40, `feedback` 0 and `mix` 40; a hollow,
nasal tone with `feedback` -60; and a fast, bright warble with `rate` 5, `centre` 2000 and `depth` 30.

<header>Sweep</header>

All six filters share one corner frequency, at which each shifts the phase by 90 degrees.  The corner is
`centre * 2^(2 * depth / 100 * sin(phase))` Hz, so the sweep is symmetrical in octaves about the centre and covers 25
Hz to 16 kHz at the widest settings.  The notches lie at about 0.27, 1 and 3.7 times the corner, drawing closer
together as they approach the Nyquist frequency.  The corner is limited to between 20 Hz and 45 percent of the output
rate.  At output rates below 35.6 kHz the upper part of the widest sweeps is therefore held at the limit, which is
3600 Hz at an 8 kHz output.

Both channels follow the same oscillator, so the stereo image is preserved and no audio passes between the channels.
A mono output is processed as a single channel.  The oscillator phase is zero whenever the phaser is reset, so the
sweep starts at the centre and rises first.  It advances once per output frame while audio is being processed and
does not advance while the chain is idle or bypassed.

<header>Levels and Headroom</header>

Levels are measured in dBFS, where a sample magnitude of 1.0 is 0 dBFS, independently of the output format.  Without
feedback the filtered signal has the same level as the input at every frequency, and the weighted dry-wet blend can
only preserve or cut that level.  Feedback adds resonant peaks:
at a steady corner, the filtered signal can reach `1 / (1 - |feedback|)` times the input, which is five times
(14 dB) at the 80 percent limit.  A moving sweep can briefly exceed this.  The phaser is stable for every supported
setting, including while the sweep and parameters change, but tonal material can exceed 0 dBFS at high feedback.
Lower the input level or follow the phaser with a limiter when high feedback is required.  Non-finite samples are
outside the normalised mixing contract and are treated as silence.

<header>Live Changes</header>

Changes to every parameter ramp linearly over 10 ms from their current values.  The rate ramps as a frequency, so
the oscillator continues smoothly from its current phase, and the centre ramps in octaves.  Flush preserves the
filter history and the oscillator phase.  Setting the mix to zero leaves the phaser running, so raising it again
reveals the filtered audio that is present.

Device reactivation, rate or layout changes and leaving bypass discard the filter history, return the oscillator
phase to zero and apply the latest parameters without a transition.

<header>Tail and Drain</header>

The phaser has zero algorithmic latency.  The all-pass filters delay low frequencies by more than high ones, most
strongly near the lowest corners, but this is the effect itself and is not compensated.  After its input stops, the
filters and the feedback loop ring down.  The phaser remains pending until the energy held in its filters can no
longer produce output within 120 dB of the loudest input, and then becomes idle.  This is typically tens of
milliseconds; with low corners and high feedback it can take a few seconds.  A running oscillator does not keep the
phaser pending.  The decay estimate used when other audio shares the phaser covers a 60 dB decay at the lowest
corner that the current settings reach.  Hard bypass and destruction cut the tail abruptly.

The phaser reports a conservative tail bound to @Audio, which uses it to decide when a drain has run longer than
expected.  The bound is proven for any sequence of supported settings, so it is longer than the actual tail; at the
lowest corners and maximum feedback it is about 12 seconds.

Output rates from 8000 to 192000 Hz are supported.  Configuring the effect for any other rate fails with
`ERR::NoSupport`.  The phaser holds no delay memory: its state is fixed at fourteen values.

-END-

*********************************************************************************************************************/

#include "audio_phaser_dsp.h"

class extAudioPhaser : public extAudioEffect {
public:
   PhaserSettings Settings;
   PhaserProcessor *Processor = nullptr;

   extAudioPhaser(objMetaClass *ClassPtr, OBJECTID ObjectID);
};

//********************************************************************************************************************
// Parameter descriptors.  Order matches the PH_ indexes.

enum { PH_RATE = 0, PH_CENTRE, PH_DEPTH, PH_FEEDBACK, PH_MIX };

static const AudioParamDesc glPhaserParams[] = {
   { .Key = "rate", .Label = "Rate",
     .Description = "The frequency of the sweep.",
     .Unit = APU::HZ, .Scale = APS::LOG, .Min = 0.02, .Max = 10, .Default = 0.4 },
   { .Key = "centre", .Label = "Centre",
     .Description = "The frequency at the geometric centre of the sweep.",
     .Unit = APU::HZ, .Scale = APS::LOG, .Min = 100, .Max = 4000, .Default = 800 },
   { .Key = "depth", .Label = "Depth",
     .Description = "How far the sweep travels either side of the centre, where 100% is two octaves.  Zero holds the "
        "notches still.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 100, .Default = 60 },
   { .Key = "feedback", .Label = "Feedback",
     .Description = "The signed proportion of the filtered signal fed back into the filters.  Higher magnitudes give "
        "stronger resonance; negative values move the resonant peaks.",
     .Unit = APU::PERCENT, .Min = -80, .Max = 80, .Default = 20 },
   { .Key = "mix", .Label = "Mix",
     .Description = "The proportion of the filtered signal in the output.  Zero leaves the input unchanged and 50% "
        "gives the deepest notches.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 100, .Default = 50 }
};

static const AudioOutputDesc glPhaserOutputs[] = {
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", "Maximum input sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", "Maximum output sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::OUTPUT_PEAK }
};

//********************************************************************************************************************

static PhaserSettings phaser_settings(const AudioParamState &State)
{
   return PhaserSettings {
      .Rate     = State.Params[PH_RATE],
      .Centre   = State.Params[PH_CENTRE],
      .Depth    = State.Params[PH_DEPTH],
      .Feedback = State.Params[PH_FEEDBACK],
      .Mix      = State.Params[PH_MIX]
   };
}

static void phaser_read(extAudioEffect *Effect, AudioParamState &State)
{
   const auto &settings = ((extAudioPhaser *)Effect)->Settings;
   State.Params.assign({ settings.Rate, settings.Centre, settings.Depth, settings.Feedback, settings.Mix });
   State.Groups.clear();
}

//********************************************************************************************************************
// Before initialisation there is no processor.

static void phaser_apply(extAudioEffect *Effect, const AudioParamState &State)
{
   ((extAudioPhaser *)Effect)->Settings = phaser_settings(State);
}

//********************************************************************************************************************
// Targets are derived outside the mixer lock.  An unsupported rate leaves the target invalid, in which case only the
// settings are published and the processor remains inactive.

class PhaserUpdate final : public AudioParamUpdate {
public:
   PhaserSettings Settings;
   PhaserTarget Target;
   bool Valid;

   PhaserUpdate(const AudioParamState &State, int Rate) : Settings(phaser_settings(State)) {
      Valid = phaser_target(Settings, Rate, Target);
   }

   void publish(extAudioEffect *Effect) override {
      auto Self = (extAudioPhaser *)Effect;
      if (Self->Processor) Self->Processor->update(Settings, Valid ? &Target : nullptr);
      Self->Settings = Settings;
   }
};

static std::unique_ptr<AudioParamUpdate> phaser_prepare(extAudioEffect *Effect, const AudioParamState &State,
   int Rate)
{
   return std::make_unique<PhaserUpdate>(State, Rate);
}

//********************************************************************************************************************

static const AudioEffectSchema glPhaserSchema = {
   .ClassName   = "AudioPhaser",
   .Version     = 1,
   .Description = "A phaser that sweeps a set of notches through the sound by mixing it with an all-pass filtered "
      "copy.",
   .Params      = glPhaserParams,
   .Outputs     = glPhaserOutputs,
   .Read        = phaser_read,
   .Apply       = phaser_apply,
   .Prepare     = phaser_prepare
};

//********************************************************************************************************************

extAudioPhaser::extAudioPhaser(objMetaClass *ClassPtr, OBJECTID ObjectID) : extAudioEffect(ClassPtr, ObjectID)
{
   Schema = &glPhaserSchema;
}

//********************************************************************************************************************

static ERR AUDIOPHASER_Init(extAudioPhaser *Self)
{
   if (Self->Processor) return ERR::InvalidState;
   auto chain = Self->Chain.lock();
   if (not chain) return ERR::NotInitialised;

   AudioParamState state;
   phaser_read(Self, state);
   {
      std::lock_guard mixer_lock(*chain->Mutex);
      if (validate_state(glPhaserSchema, state, Self->OutputRate) != ERR::Okay) return ERR::InvalidValue;
   }

   auto processor = std::make_unique<PhaserProcessor>(Self, Self->Settings);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;

   return error;
}

//********************************************************************************************************************

#include "class_audiophaser_def.c"

//********************************************************************************************************************

static ERR add_audiophaser_class()
{
   clAudioPhaser = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIOEFFECT),
      fl::ClassID(CLASSID::AUDIOPHASER),
      fl::Name("AudioPhaser"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioPhaserActions),
      fl::Size(sizeof(extAudioPhaser)),
      fl::Path(MOD_PATH));
   return clAudioPhaser ? ERR::Okay : ERR::AddClass;
}
