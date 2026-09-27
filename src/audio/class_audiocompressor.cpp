/*********************************************************************************************************************

-CLASS-
AudioCompressor: A peak compressor that reduces the dynamic range of audio above a threshold.

The compressor reduces the level of audio that exceeds a threshold, making quiet and loud passages more consistent.
It is commonly used to improve the intelligibility of dialogue and to control the dynamics of mixed playback.

Create new compressor objects as a child of an @Audio object or set the inherited #Audio field.  Use #Channel
to process one channel set, or leave it at zero to process the global mix.  Application compressors accept live
changes; global compressors become immutable after initialisation.

Parameters are published through the inherited @AudioEffect schema and are changed with @AudioEffect.SetParameter()
followed by @AudioEffect.Flush():

<list type="bullet">
<li>`threshold`: the detector level, in dB relative to full scale, above which compression is applied.</li>
<li>`ratio`: the slope of the transfer curve above the threshold.  At 4, a signal that rises 4 dB above the threshold
leaves the compressor only 1 dB above it.  A ratio of 1 disables compression.</li>
<li>`attack`: the time constant, in milliseconds, with which gain reduction increases.</li>
<li>`release`: the time constant, in milliseconds, with which gain reduction recovers.</li>
<li>`knee`: the width, in dB, of a soft transition centred on the threshold.  Zero selects a hard knee.</li>
<li>`makeup`: a gain, in dB, that is applied to the compressed signal to restore its loudness.</li>
<li>`mix`: the linear blend of the dry input and the compressed signal.  Zero passes the input unchanged and values
between zero and 100 percent provide parallel compression.</li>
</list>

The following compresses dialogue on a playing channel set and adds makeup gain as a single change:

<pre>
comp = obj.new('AudioCompressor', { audio=audio, channel=channel })
comp.mtSetParameter('threshold', -24)
comp.mtSetParameter('ratio', 3)
comp.mtSetParameter('makeup', 6)
comp.acFlush()
</pre>

<header>Detection and Transfer Curve</header>

The detector measures the absolute value of every sample.  In stereo, the greater of the two channel magnitudes is
used and the resulting gain is applied equally to both channels, so the louder channel controls compression and the
stereo image does not move.  Levels are measured in dBFS, where a magnitude of 1.0 is 0 dBFS, independently of the
output format.

For a detector level `x`, threshold `T`, ratio `R` and knee width `W`, the target gain reduction `g` in dB is zero
below the knee and `(1 - 1/R) * (x - T)` above it.  Within a soft knee, where `x` is between `T - W/2` and `T + W/2`,
the reduction is `(1 - 1/R) * (x - T + W/2)^2 / 2W`.  This curve is continuous in value and slope at both edges of
the knee.

<header>Timing</header>

The target reduction is smoothed in the dB domain.  When the target exceeds the current reduction the attack time
constant is used, otherwise the release time constant.  Each is an exponential time constant: after that duration,
63.2% of a change in the target has been traversed.

The compressor has zero lookahead and zero latency.  The gain for each sample is computed from that sample, so the
leading edge of a transient passes before the attack has taken effect and can exceed the static transfer curve.  Place
a limiter later in the chain if a strict peak ceiling is required.  The compressor never produces output in response
to silence, so it has no tail and never delays the completion of a sound.

<header>Makeup and Mix</header>

The compressed branch is `input * gain * makeup` and the output is `input + mix * (compressed - input)`, where `mix`
is a proportion from 0 to 1.  This is a linear amplitude crossfade.  Makeup gain does not affect the detector or the
reported gain reduction.  Makeup gain and parallel mixing can raise the output above 0 dBFS; the mixer retains that
headroom until final conversion to the output format.

<header>Meters</header>

In addition to the standard input and output peaks for each channel, the compressor publishes a `gain_reduction`
meter.  It reports the greatest gain reduction, in dB, reached during the meter interval, before makeup gain and
dry/wet mixing.  A reading of zero means that no attenuation was applied.

<header>Live Changes</header>

Changes to every parameter ramp linearly over 10 ms, so edits do not click, and the gain reduction then follows the
new transfer curve at the current attack and release rates.  An edit that arrives during a ramp starts a new ramp from
the current values.  Device reactivation, leaving bypass and reaching idle discard the gain reduction and apply the
latest parameters without a transition.

Output rates from 8000 to 192000 Hz are supported.  Configuring the effect for any other rate fails with
`ERR::NoSupport`.

-END-

*********************************************************************************************************************/

#include "audio_compressor_dsp.h"

class extAudioCompressor : public extAudioEffect {
public:
   CompressorSettings Settings;
   CompressorProcessor *Processor = nullptr;

   extAudioCompressor(objMetaClass *ClassPtr, OBJECTID ObjectID);
};

//********************************************************************************************************************
// Parameter descriptors.  Order matches the CP_ indexes.

enum { CP_THRESHOLD = 0, CP_RATIO, CP_ATTACK, CP_RELEASE, CP_KNEE, CP_MAKEUP, CP_MIX };

static const AudioParamDesc glCompressorParams[] = {
   { .Key = "threshold", .Label = "Threshold",
     .Description = "The detector level, relative to full scale, above which the signal is compressed.",
     .Unit = APU::DB, .Min = -60, .Max = 0, .Default = -18 },
   { .Key = "ratio", .Label = "Ratio",
     .Description = "The ratio of the input level to the output level above the threshold.  A ratio of 1 disables "
        "compression.",
     .Unit = APU::RATIO, .Scale = APS::LOG, .Min = 1, .Max = 20, .Default = 4 },
   { .Key = "attack", .Label = "Attack",
     .Description = "The time constant with which gain reduction increases when the level rises.",
     .Unit = APU::MS, .Scale = APS::LOG, .Min = 0.1, .Max = 200, .Default = 10 },
   { .Key = "release", .Label = "Release",
     .Description = "The time constant with which gain reduction recovers when the level falls.",
     .Unit = APU::MS, .Scale = APS::LOG, .Min = 5, .Max = 2000, .Default = 100 },
   { .Key = "knee", .Label = "Knee",
     .Description = "The width of a gradual transition centred on the threshold.  Zero selects a hard knee.",
     .Unit = APU::DB, .Min = 0, .Max = 24, .Default = 6 },
   { .Key = "makeup", .Label = "Makeup",
     .Description = "Gain applied to the compressed signal to restore its loudness.",
     .Unit = APU::DB, .Min = -24, .Max = 24, .Default = 0 },
   { .Key = "mix", .Label = "Mix",
     .Description = "The proportion of compressed signal in the output.  Lower values provide parallel "
        "compression and zero leaves the input unchanged.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 100, .Default = 100 }
};

static const AudioOutputDesc glCompressorOutputs[] = {
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", "Maximum input sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", "Maximum output sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::OUTPUT_PEAK },
   { "gain_reduction", AudioOutputKind::SCALAR, "Gain Reduction",
      "Maximum attenuation applied by the compressor, before makeup gain and mixing.", "dB", "global",
      "maximum-attenuation", AudioMeterSource::GAIN_REDUCTION }
};

//********************************************************************************************************************

static CompressorSettings compressor_settings(const AudioParamState &State)
{
   return CompressorSettings {
      .Threshold = State.Params[CP_THRESHOLD],
      .Ratio     = State.Params[CP_RATIO],
      .Attack    = State.Params[CP_ATTACK],
      .Release   = State.Params[CP_RELEASE],
      .Knee      = State.Params[CP_KNEE],
      .Makeup    = State.Params[CP_MAKEUP],
      .Mix       = State.Params[CP_MIX]
   };
}

static void compressor_read(extAudioEffect *Effect, AudioParamState &State)
{
   const auto &settings = ((extAudioCompressor *)Effect)->Settings;
   State.Params.assign({ settings.Threshold, settings.Ratio, settings.Attack, settings.Release, settings.Knee,
      settings.Makeup, settings.Mix });
   State.Groups.clear();
}

//********************************************************************************************************************
// Before initialisation there is no processor.

static void compressor_apply(extAudioEffect *Effect, const AudioParamState &State)
{
   ((extAudioCompressor *)Effect)->Settings = compressor_settings(State);
}

//********************************************************************************************************************
// The target is derived outside the mixer lock.  An unsupported rate leaves the target invalid, in which case only
// the settings are published and the processor remains inactive.

class CompressorUpdate final : public AudioParamUpdate {
public:
   CompressorSettings Settings;
   CompressorTarget Target;
   bool Valid;

   CompressorUpdate(const AudioParamState &State, int Rate) : Settings(compressor_settings(State)) {
      Valid = compressor_target(Settings, Rate, Target);
   }

   void publish(extAudioEffect *Effect) override {
      auto Self = (extAudioCompressor *)Effect;
      if (Self->Processor) Self->Processor->update(Settings, Valid ? &Target : nullptr);
      Self->Settings = Settings;
   }
};

static std::unique_ptr<AudioParamUpdate> compressor_prepare(extAudioEffect *Effect, const AudioParamState &State,
   int Rate)
{
   return std::make_unique<CompressorUpdate>(State, Rate);
}

//********************************************************************************************************************

static const AudioEffectSchema glCompressorSchema = {
   .ClassName   = "AudioCompressor",
   .Version     = 1,
   .Description = "A peak compressor that reduces the dynamic range of audio above a threshold.",
   .Params      = glCompressorParams,
   .Outputs     = glCompressorOutputs,
   .Read        = compressor_read,
   .Apply       = compressor_apply,
   .Prepare     = compressor_prepare
};

//********************************************************************************************************************

extAudioCompressor::extAudioCompressor(objMetaClass *ClassPtr, OBJECTID ObjectID) : extAudioEffect(ClassPtr, ObjectID)
{
   Schema = &glCompressorSchema;
}

//********************************************************************************************************************

static ERR AUDIOCOMPRESSOR_Init(extAudioCompressor *Self)
{
   if (Self->Processor) return ERR::InvalidState;
   auto chain = Self->Chain.lock();
   if (not chain) return ERR::NotInitialised;

   AudioParamState state;
   compressor_read(Self, state);
   {
      std::lock_guard mixer_lock(*chain->Mutex);
      if (validate_state(glCompressorSchema, state, Self->OutputRate) != ERR::Okay) return ERR::InvalidValue;
   }

   auto processor = std::make_unique<CompressorProcessor>(Self, Self->Settings);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;

   return error;
}

//********************************************************************************************************************

#include "class_audiocompressor_def.c"

static const FieldArray clAudioCompressorFields[] = {
   END_FIELD
};

//********************************************************************************************************************

static ERR add_audiocompressor_class()
{
   clAudioCompressor = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIOEFFECT),
      fl::ClassID(CLASSID::AUDIOCOMPRESSOR),
      fl::Name("AudioCompressor"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioCompressorActions),
      fl::Fields(clAudioCompressorFields),
      fl::Size(sizeof(extAudioCompressor)),
      fl::Path(MOD_PATH));
   return clAudioCompressor ? ERR::Okay : ERR::AddClass;
}
