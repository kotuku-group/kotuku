/*********************************************************************************************************************

-CLASS-
AudioLimiter: A lookahead peak limiter that holds audio below a ceiling.

The limiter prevents the sample peaks of its output from exceeding a ceiling, while leaving audio below the ceiling
unchanged.  It is commonly placed at the end of a chain to protect against clipping after effects that add gain,
such as an equaliser, a compressor with makeup gain, or a reverb.

Create new limiter objects as a child of an @Audio object or set the inherited #AudioEffect.Audio field.  Use
#AudioEffect.Channel to process one channel set, or leave it at zero to process the global mix.  Application
limiters accept live changes; global limiters become immutable after initialisation.

Parameters are published through the inherited @AudioEffect schema and are changed with @AudioEffect.SetKey()
followed by @AudioEffect.Flush():

<list type="bullet">
<li>`ceiling`: the greatest permitted output sample magnitude, in dB relative to full scale, from -24 to 0 dB.</li>
<li>`release`: the time constant, in milliseconds, with which gain reduction recovers after a peak has passed.</li>
<li>`gain`: a gain, in dB, that is applied to the input before peak detection.  Positive values raise the loudness
of the signal and the limiter holds the result below the ceiling.</li>
</list>

The following limits a playing channel set, raising its level by 6 dB with a ceiling of -0.5 dB, as a single change:

<pre>
limiter = obj.new('AudioLimiter', { audio=audio, channel=channel })
limiter.acSetKey('ceiling', -0.5)
limiter.acSetKey('gain', 6)
limiter.acFlush()
</pre>

<header>Detection and Ceiling</header>

Levels are measured in dBFS, where a sample magnitude of 1.0 is 0 dBFS, independently of the output format.  The
detector takes the absolute value of every sample after the input gain.  In stereo, the greater of the two channel
magnitudes is used and the same gain is applied to both channels, so the louder channel controls limiting and the
stereo image and polarity of each channel are preserved.

The input is delayed by a fixed lookahead of 5 ms, rounded up to whole frames at the output rate: 221 frames at
44.1 kHz, 240 at 48 kHz and 480 at 96 kHz.  The gain reduction for each output frame is the greatest reduction
required by any peak from that frame through the newest input, so attenuation is complete before a peak leaves the
delay and is held until the peak has passed.  Reduction increases instantly and recovers exponentially in the dB
domain: after one release time, 36.8% of the reduction remains when no peak continues to demand it.

Every output sample is at or below the ceiling, including samples that arrive above 0 dBFS, the start of playback,
recovery and live parameter changes.  This is a sample-peak limiter: it does not measure peaks between samples, so the
reconstructed analogue signal can exceed the ceiling slightly.  The guarantee applies to the limiter's own output;
effects later in the chain and the output filter of the @Audio object can raise peaks again.  Place the limiter last
in a chain when a strict ceiling is required.

Non-finite samples are outside the normalised mixing contract and are treated as silence.

<header>Latency and Drain</header>

The lookahead is published in #AudioEffect.Latency and does not change unless the output is reconfigured at another
rate.  The first lookahead interval of output after activation, leaving bypass or idle is silent.  The limiter has no
decaying tail, but remains pending after its input stops until the last buffered sample has left the delay, so the
end of a sound is never cut short.

<header>Meters</header>

In addition to the standard input and output peaks for each channel, the limiter publishes a `gain_reduction`
meter.  It reports the greatest attenuation, in dB, applied during the meter interval, excluding the input gain.
Input peaks are measured before the input gain and output peaks after limiting.

<header>Live Changes</header>

Changes to the input gain and release time ramp linearly over 10 ms.  A ceiling change takes effect immediately for
every sample that has not yet left the delay, so lowering it never allows a buffered peak through.  When the ceiling
is raised, the gain reduction recovers at the release rate rather than jumping.  Device reactivation, leaving bypass
and reaching idle discard buffered audio and gain reduction and apply the latest parameters without a transition.

Output rates from 8000 to 192000 Hz are supported.  Configuring the effect for any other rate fails with
`ERR::NoSupport`.

-END-

*********************************************************************************************************************/

#include "audio_limiter_dsp.h"

class extAudioLimiter : public extAudioEffect {
public:
   LimiterSettings Settings;
   LimiterProcessor *Processor = nullptr;

   extAudioLimiter(objMetaClass *ClassPtr, OBJECTID ObjectID);
};

//********************************************************************************************************************
// Parameter descriptors.  Order matches the LM_ indexes.

enum { LM_CEILING = 0, LM_RELEASE, LM_GAIN };

static const AudioParamDesc glLimiterParams[] = {
   { .Key = "ceiling", .Label = "Ceiling",
     .Description = "The greatest permitted output sample magnitude, relative to full scale.",
     .Unit = APU::DB, .Min = -24, .Max = 0, .Default = -1 },
   { .Key = "release", .Label = "Release",
     .Description = "The time constant with which gain reduction recovers after a peak has passed.",
     .Unit = APU::MS, .Scale = APS::LOG, .Min = 5, .Max = 1000, .Default = 100 },
   { .Key = "gain", .Label = "Gain",
     .Description = "Gain applied to the input before peak detection and limiting.",
     .Unit = APU::DB, .Min = -24, .Max = 24, .Default = 0 }
};

static const AudioOutputDesc glLimiterOutputs[] = {
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", "Maximum input sample peak, before the input gain.",
      "dBFS", "channel", "sample-peak", AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", "Maximum output sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::OUTPUT_PEAK },
   { "gain_reduction", AudioOutputKind::SCALAR, "Gain Reduction",
      "Maximum attenuation applied by the limiter, excluding the input gain.", "dB", "global",
      "maximum-attenuation", AudioMeterSource::GAIN_REDUCTION }
};

//********************************************************************************************************************

static LimiterSettings limiter_settings(const AudioParamState &State)
{
   return LimiterSettings {
      .Ceiling = State.Params[LM_CEILING],
      .Release = State.Params[LM_RELEASE],
      .Gain    = State.Params[LM_GAIN]
   };
}

static void limiter_read(extAudioEffect *Effect, AudioParamState &State)
{
   const auto &settings = ((extAudioLimiter *)Effect)->Settings;
   State.Params.assign({ settings.Ceiling, settings.Release, settings.Gain });
   State.Groups.clear();
}

//********************************************************************************************************************
// Before initialisation there is no processor.

static void limiter_apply(extAudioEffect *Effect, const AudioParamState &State)
{
   ((extAudioLimiter *)Effect)->Settings = limiter_settings(State);
}

//********************************************************************************************************************
// The target is derived outside the mixer lock.  An unsupported rate leaves the target invalid, in which case only
// the settings are published and the processor remains inactive.  Edits never change the latency.

class LimiterUpdate final : public AudioParamUpdate {
public:
   LimiterSettings Settings;
   LimiterTarget Target;
   bool Valid;

   LimiterUpdate(const AudioParamState &State, int Rate) : Settings(limiter_settings(State)) {
      Valid = limiter_target(Settings, Rate, Target);
   }

   void publish(extAudioEffect *Effect) override {
      auto Self = (extAudioLimiter *)Effect;
      if (Self->Processor) Self->Processor->update(Settings, Valid ? &Target : nullptr);
      Self->Settings = Settings;
   }
};

static std::unique_ptr<AudioParamUpdate> limiter_prepare(extAudioEffect *Effect, const AudioParamState &State,
   int Rate)
{
   return std::make_unique<LimiterUpdate>(State, Rate);
}

//********************************************************************************************************************

static const AudioEffectSchema glLimiterSchema = {
   .ClassName   = "AudioLimiter",
   .Version     = 1,
   .Description = "A lookahead peak limiter that holds audio below a ceiling.",
   .Params      = glLimiterParams,
   .Outputs     = glLimiterOutputs,
   .Read        = limiter_read,
   .Apply       = limiter_apply,
   .Prepare     = limiter_prepare
};

//********************************************************************************************************************

extAudioLimiter::extAudioLimiter(objMetaClass *ClassPtr, OBJECTID ObjectID) : extAudioEffect(ClassPtr, ObjectID)
{
   Schema = &glLimiterSchema;
}

//********************************************************************************************************************
// Storage is allocated by set_processor() outside the mixer lock.

static ERR AUDIOLIMITER_Init(extAudioLimiter *Self)
{
   if (Self->Processor) return ERR::InvalidState;
   auto chain = Self->Chain.lock();
   if (not chain) return ERR::NotInitialised;

   AudioParamState state;
   limiter_read(Self, state);
   {
      std::lock_guard mixer_lock(*chain->Mutex);
      if (validate_state(glLimiterSchema, state, Self->OutputRate) != ERR::Okay) return ERR::InvalidValue;
   }

   auto processor = std::make_unique<LimiterProcessor>(Self, Self->Settings);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;

   return error;
}

//********************************************************************************************************************

#include "class_audiolimiter_def.c"

//********************************************************************************************************************

static ERR add_audiolimiter_class()
{
   clAudioLimiter = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIOEFFECT),
      fl::ClassID(CLASSID::AUDIOLIMITER),
      fl::Name("AudioLimiter"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioLimiterActions),
      fl::Size(sizeof(extAudioLimiter)),
      fl::Path(MOD_PATH));
   return clAudioLimiter ? ERR::Okay : ERR::AddClass;
}
