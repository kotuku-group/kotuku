/*********************************************************************************************************************

-CLASS-
AudioCrossfeed: Blends a filtered portion of each stereo channel into the opposite ear for headphone listening.

When stereo recordings are played through loudspeakers, each ear hears both speakers: the far speaker arrives slightly
later and with its treble shaded by the head.  Headphones remove that acoustic crosstalk, so sounds that are panned
hard to one side are heard in one ear only, which can be fatiguing and places the image inside the head.  The crossfeed
effect restores a simplified form of the crosstalk by mixing a low-passed and slightly delayed copy of each channel
into the other.

This is a simple crossfeed and not HRTF spatialisation: it does not model the outer ear, room reflections or
elevation, and it does not externalise sound the way a binaural renderer can.

Create new crossfeed objects as a child of an @Audio object or set the inherited #AudioEffect.Audio field.  Use
#AudioEffect.Channel to process one channel set, or leave it at zero to process the global mix.  Application effects
accept live changes; global effects become immutable after initialisation.

Parameters are published through the inherited @AudioEffect schema and are changed with @AudioEffect.SetKey()
followed by @AudioEffect.Flush():

<list type="bullet">
<li>`amount`: the level of the opposite channel's contribution, from 0 to 50 percent.  Zero disables the crossfeed.</li>
<li>`cutoff`: the low-pass corner of the crossfed signal, from 100 to 2000 Hz.  Lower values leave only the bass in
the opposite ear.</li>
<li>`delay`: the additional delay of the crossfed signal, from 0 to 1 ms.  The distance between the ears gives about
0.3 ms.</li>
<li>`gain`: the output trim, from -12 to 12 dB.</li>
</list>

The defaults (20 percent, 700 Hz and 0.3 ms) are a moderate setting suitable for most music.  The following gives a
stronger effect for recordings with hard-panned instruments, as a single change:

<pre>
crossfeed = obj.new('AudioCrossfeed', { audio=audio, channel=channel })
crossfeed.acSetKey('amount', 35)
crossfeed.acSetKey('cutoff', 650)
crossfeed.acFlush()
</pre>

Other useful combinations include a subtle setting with `amount` 10 and `cutoff` 700, and a bass-only blend with
`amount` 30 and `cutoff` 300.

<header>Processing</header>

For left and right inputs `L` and `R`, with `a` the amount divided by 100, the outputs are
`(L + a * LP(R)) / (1 + a)` and `(R + a * LP(L)) / (1 + a)`, delayed in the crossfed term only, followed by the trim.
`LP` is a first-order low-pass at the cutoff, which falls by 6 dB per octave above it.

The division by `1 + a` keeps low-frequency sounds at the centre of the image at their original level, because both
channels receive their own signal plus `a` times the other.  Above the cutoff the crossfed term fades, so centred
treble settles at `1 / (1 + a)` of its level: -1.6 dB at the default amount and -3.5 dB at 50 percent.  Around the
cutoff, the phase shift of the filtered and delayed term reduces centred content a little further.  At the defaults it
is -0.6 dB at 300 Hz and -2.2 dB at 700 Hz, and with 50 percent, a 100 Hz cutoff and a 1 ms delay it reaches -5 dB at
300 Hz.  Sounds panned to one side are reduced by `1 + a` in their own channel and appear in the other at up to
`a / (1 + a)` of their level.  Raise `gain` to compensate if required.

A mono output has no stereo image.  The amount, cutoff and delay have no effect and only the trim is applied.  With
`amount` at zero and `gain` at zero the output is identical to the input.

The low-pass corner is limited to 45 percent of the output rate, as for @AudioEqualiser filters, while the requested
value is retained.  The highest cutoff is below this limit at every supported rate, so no clamping occurs in practice.

<header>Levels and Headroom</header>

Levels are measured in dBFS, where a sample magnitude of 1.0 is 0 dBFS, independently of the output format.  The
filter and the delay interpolation only average past values, so before the trim neither output can exceed the peak
input.  Non-finite samples are outside the normalised mixing contract and are treated as silence.

<header>Live Changes</header>

Changes to `amount`, `cutoff` and `gain` ramp linearly over 10 ms from their current values; the cutoff ramps the filter
pole, which keeps the low-frequency level constant throughout.  A change to `delay` crossfades from the old delay to
the new one over 10 ms; further changes during a crossfade take effect when it ends.  Flush preserves the filter and
delay history.

Device reactivation, rate or layout changes and leaving bypass discard the history and apply the latest parameters
without a transition.

<header>Tail and Drain</header>

The effect has zero latency because the direct signal is never delayed.  The crossfed signal continues briefly after
the input stops: the effect remains pending until the delayed and filtered history is 140 dB below the loudest input,
which takes 4 to 5 ms at the default cutoff and up to 25 ms at the lowest cutoff with the longest delay.  With `amount`
at zero, or with a mono output, there is no tail.  Hard bypass and destruction cut the tail abruptly.

Output rates from 8000 to 192000 Hz are supported.  Configuring the effect for any other rate fails with
`ERR::NoSupport`.  The effect allocates no audio memory.

-END-

*********************************************************************************************************************/

#include "audio_crossfeed_dsp.h"

class extAudioCrossfeed : public extAudioEffect {
public:
   CrossfeedSettings Settings;
   CrossfeedProcessor *Processor = nullptr;

   extAudioCrossfeed(objMetaClass *ClassPtr, OBJECTID ObjectID);
};

//********************************************************************************************************************
// Parameter descriptors.  Order matches the CF_ indexes.

enum { CF_AMOUNT = 0, CF_CUTOFF, CF_DELAY, CF_GAIN };

static const AudioParamDesc glCrossfeedParams[] = {
   { .Key = "amount", .Label = "Amount",
     .Description = "The level of the opposite channel's contribution.  Zero disables the crossfeed.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 50, .Default = 20 },
   { .Key = "cutoff", .Label = "Cutoff",
     .Description = "The low-pass corner of the crossfed signal.",
     .Unit = APU::HZ, .Scale = APS::LOG, .Min = 100, .Max = 2000, .Default = 700 },
   { .Key = "delay", .Label = "Delay",
     .Description = "The additional delay of the crossfed signal.",
     .Unit = APU::MS, .Min = 0, .Max = CROSSFEED_MAX_DELAY, .Default = 0.3 },
   { .Key = "gain", .Label = "Gain",
     .Description = "The output trim.",
     .Unit = APU::DB, .Min = -12, .Max = 12, .Default = 0 }
};

static const AudioOutputDesc glCrossfeedOutputs[] = {
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", "Maximum input sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", "Maximum output sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::OUTPUT_PEAK }
};

//********************************************************************************************************************

static CrossfeedSettings crossfeed_settings(const AudioParamState &State)
{
   return CrossfeedSettings {
      .Amount = State.Params[CF_AMOUNT],
      .Cutoff = State.Params[CF_CUTOFF],
      .Delay  = State.Params[CF_DELAY],
      .Gain   = State.Params[CF_GAIN]
   };
}

static void crossfeed_read(extAudioEffect *Effect, AudioParamState &State)
{
   const auto &settings = ((extAudioCrossfeed *)Effect)->Settings;
   State.Params.assign({ settings.Amount, settings.Cutoff, settings.Delay, settings.Gain });
   State.Groups.clear();
}

//********************************************************************************************************************
// Before initialisation there is no processor.

static void crossfeed_apply(extAudioEffect *Effect, const AudioParamState &State)
{
   ((extAudioCrossfeed *)Effect)->Settings = crossfeed_settings(State);
}

//********************************************************************************************************************
// Targets are derived outside the mixer lock.  An unsupported rate leaves the target invalid, in which case only the
// settings are published and the processor remains inactive.

class CrossfeedUpdate final : public AudioParamUpdate {
public:
   CrossfeedSettings Settings;
   CrossfeedTarget Target;
   bool Valid;

   CrossfeedUpdate(const AudioParamState &State, int Rate) : Settings(crossfeed_settings(State)) {
      Valid = crossfeed_target(Settings, Rate, Target);
   }

   void publish(extAudioEffect *Effect) override {
      auto Self = (extAudioCrossfeed *)Effect;
      if (Self->Processor) Self->Processor->update(Settings, Valid ? &Target : nullptr);
      Self->Settings = Settings;
   }
};

static std::unique_ptr<AudioParamUpdate> crossfeed_prepare(extAudioEffect *Effect, const AudioParamState &State,
   int Rate)
{
   return std::make_unique<CrossfeedUpdate>(State, Rate);
}

//********************************************************************************************************************

static const AudioEffectSchema glCrossfeedSchema = {
   .ClassName   = "AudioCrossfeed",
   .Version     = 1,
   .Description = "Blends a filtered portion of each stereo channel into the opposite ear for headphone listening.",
   .Params      = glCrossfeedParams,
   .Outputs     = glCrossfeedOutputs,
   .Read        = crossfeed_read,
   .Apply       = crossfeed_apply,
   .Prepare     = crossfeed_prepare
};

//********************************************************************************************************************

extAudioCrossfeed::extAudioCrossfeed(objMetaClass *ClassPtr, OBJECTID ObjectID) : extAudioEffect(ClassPtr, ObjectID)
{
   Schema = &glCrossfeedSchema;
}

//********************************************************************************************************************

static ERR AUDIOCROSSFEED_Init(extAudioCrossfeed *Self)
{
   if (Self->Processor) return ERR::InvalidState;
   auto chain = Self->Chain.lock();
   if (not chain) return ERR::NotInitialised;

   AudioParamState state;
   crossfeed_read(Self, state);
   {
      std::lock_guard mixer_lock(*chain->Mutex);
      if (validate_state(glCrossfeedSchema, state, Self->OutputRate) != ERR::Okay) return ERR::InvalidValue;
   }

   auto processor = std::make_unique<CrossfeedProcessor>(Self, Self->Settings);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;

   return error;
}

//********************************************************************************************************************

#include "class_audiocrossfeed_def.c"

//********************************************************************************************************************

static ERR add_audiocrossfeed_class()
{
   clAudioCrossfeed = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIOEFFECT),
      fl::ClassID(CLASSID::AUDIOCROSSFEED),
      fl::Name("AudioCrossfeed"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioCrossfeedActions),
      fl::Size(sizeof(extAudioCrossfeed)),
      fl::Path(MOD_PATH));
   return clAudioCrossfeed ? ERR::Okay : ERR::AddClass;
}
