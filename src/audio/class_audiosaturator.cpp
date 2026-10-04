/*********************************************************************************************************************

-CLASS-
AudioSaturator: A waveshaper that adds harmonic colour through soft saturation or hard clipping.

The saturator amplifies its input by the `drive` level and passes the result through a fixed transfer curve that
compresses large values, which adds harmonics to the sound.  Mild settings give warmth and density; high drive
produces strong distortion.  The processed signal can be blended with the unprocessed input for parallel
saturation.

Create new saturator objects as a child of an @Audio object or set the inherited #AudioEffect.Audio field.  Use
#AudioEffect.Channel to process one channel set, or leave it at zero to process the global mix.  Application saturators
accept live changes; global saturators become immutable after initialisation.

Parameters are published through the inherited @AudioEffect schema and are changed with @AudioEffect.SetKey()
followed by @AudioEffect.Flush():

<list type="bullet">
<li>`drive`: the gain applied before the transfer curve, from 0 to 36 dB.  Higher levels push more of the signal into
the curved region and increase the distortion.</li>
<li>`shape`: `soft` (0) for smooth saturation or `hard` (1) for hard clipping.</li>
<li>`gain`: the output trim of the processed signal, from -36 to 12 dB.  It does not affect the dry signal.</li>
<li>`mix`: the linear blend of dry and processed signals, from 0 to 100 percent.  100 percent outputs only the
processed signal.</li>
</list>

The following adds heavy parallel saturation to a playing channel set, as a single change:

<pre>
saturator = obj.new('AudioSaturator', { audio=audio, channel=channel })
saturator.acSetKey('drive', 24)
saturator.acSetKey('gain', -12)
saturator.acSetKey('mix', 40)
saturator.acFlush()
</pre>

<header>Transfer Curves</header>

The processed signal is `trim * curve(drive * x)`, where `drive` and `trim` are the linear equivalents of the `drive`
and `gain` levels.  The soft curve is `tanh(z)`, which approaches ±1 smoothly.  The hard curve is `clamp(z, -1, 1)`,
which is linear below unity and flat above it.  Both are symmetric, map zero to zero and have unity slope at zero, so
quiet signals pass with the drive gain applied and the curve output never exceeds unity.  Symmetric curves add odd
harmonics; DC already present in the input is not removed.

The processed level is not compensated for the drive: raising `drive` raises the loudness as well as the distortion,
and `gain` is the means of matching levels.  At 0 dB drive the soft curve still compresses peaks, so it is not a
bypass.  Each channel is processed independently and no audio passes between channels.

<header>Oversampling and Latency</header>

The curve is applied at four times the output rate.  The input is interpolated by a 257-tap linear-phase filter
before the curve and band-limited by a matching filter afterwards, which removes most of the harmonics that would
otherwise fold back below the Nyquist frequency as inharmonic aliasing.  Oversampling reduces aliasing but cannot
remove it: high drive, hard clipping and content close to the Nyquist frequency still alias, most audibly near the
top of the band.  The combined response of the two filters is flat within 0.001 dB up to 40% of the output rate and
falls to -12 dB at 45%, and each filter attenuates by at least 100 dB from 50% of the output rate, so the processed
signal loses content in the top tenth of the band.

The filters delay the processed signal by exactly 64 output frames at every rate, which the saturator publishes as
its #AudioEffect.Latency.  The dry signal is delayed by the same 64 frames, so the two are time-aligned at every mix
setting.  Alignment matches the timing, not the frequency response: only the processed signal is band-limited.

<header>Levels and Headroom</header>

Levels are measured in dBFS, where a sample magnitude of 1.0 is 0 dBFS, independently of the output format.  The
drive and clipping thresholds do not depend on the output bit depth.  The band-limiting filter rings after the
curve, so processed peaks can exceed unity before trim: by about 0.7 dB at an isolated hard-clipped edge, and by no
more than 6.4 dB in the worst case.  Positive `gain` raises them further and the dry signal passes through
unchanged, so the saturator is not a peak limiter.  Place an @AudioLimiter after it when a sample ceiling is
required.  Non-finite samples are outside the normalised mixing contract and are treated as silence.

<header>Live Changes</header>

Changes to drive and gain ramp over 10 ms in dB, and changes to mix ramp linearly over 10 ms, from their current
values.  A change of shape crossfades the outputs of the two curves over 10 ms.  Drive and shape apply at the
oversampled rate as samples reach the curve, so samples already travelling through the interpolation filter can be
shaped with the new values; gain and mix apply at the output.  Flush preserves the filter and dry histories.  Setting
the mix to zero leaves the saturator running, with the dry signal still delayed.

Device reactivation, rate or layout changes and leaving bypass discard the histories and apply the latest parameters
without a transition.  Hard bypass removes the latency along with the processing.

<header>Tail and Drain</header>

One input frame affects 129 output frames, including the 64 frames of latency.  After its input stops, the saturator
remains pending for 128 frames and then becomes idle.  The decay estimate used when other audio shares the saturator
is the 65 frames of filter ringing beyond the latency.  Hard bypass and destruction cut the tail abruptly.

Output rates from 8000 to 192000 Hz are supported.  Configuring the effect for any other rate fails with
`ERR::NoSupport`.  The filter histories require approximately 5 KB per channel.

-END-

*********************************************************************************************************************/

#include "audio_saturator_dsp.h"

class extAudioSaturator : public extAudioEffect {
public:
   SaturatorSettings Settings;
   SaturatorProcessor *Processor = nullptr;

   extAudioSaturator(objMetaClass *ClassPtr, OBJECTID ObjectID);
};

//********************************************************************************************************************
// Parameter descriptors.  Order matches the SAT_ indexes.

enum { SAT_DRIVE = 0, SAT_SHAPE, SAT_GAIN, SAT_MIX };

static const AudioParamOption glSaturatorShapes[] = {
   { SATURATE_SOFT, "soft", "Soft", "Smooth saturation that approaches full scale gradually." },
   { SATURATE_HARD, "hard", "Hard", "Hard clipping at full scale, for a harsher sound." }
};

static const AudioParamDesc glSaturatorParams[] = {
   { .Key = "drive", .Label = "Drive",
     .Description = "The gain applied before the transfer curve.  Higher levels give more distortion.",
     .Unit = APU::DB, .Min = 0, .Max = 36, .Default = 6 },
   { .Key = "shape", .Label = "Shape",
     .Description = "Whether the transfer curve saturates smoothly or clips hard.",
     .Type = APT::ENUM, .Default = SATURATE_SOFT, .Options = glSaturatorShapes },
   { .Key = "gain", .Label = "Gain",
     .Description = "The output trim of the processed signal.  The dry signal is unaffected.",
     .Unit = APU::DB, .Min = -36, .Max = 12, .Default = -6 },
   { .Key = "mix", .Label = "Mix",
     .Description = "The proportion of the processed signal in the output.  Zero outputs the dry signal, delayed by "
        "the saturator's latency.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 100, .Default = 100 }
};

static const AudioOutputDesc glSaturatorOutputs[] = {
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", "Maximum input sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", "Maximum output sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::OUTPUT_PEAK }
};

//********************************************************************************************************************

static SaturatorSettings saturator_settings(const AudioParamState &State)
{
   return SaturatorSettings {
      .Drive = State.Params[SAT_DRIVE],
      .Shape = int(State.Params[SAT_SHAPE]),
      .Gain  = State.Params[SAT_GAIN],
      .Mix   = State.Params[SAT_MIX]
   };
}

static void saturator_read(extAudioEffect *Effect, AudioParamState &State)
{
   const auto &settings = ((extAudioSaturator *)Effect)->Settings;
   State.Params.assign({ settings.Drive, double(settings.Shape), settings.Gain, settings.Mix });
   State.Groups.clear();
}

//********************************************************************************************************************
// Before initialisation there is no processor.

static void saturator_apply(extAudioEffect *Effect, const AudioParamState &State)
{
   ((extAudioSaturator *)Effect)->Settings = saturator_settings(State);
}

//********************************************************************************************************************
// Targets are derived outside the mixer lock.  An unsupported rate leaves the target invalid, in which case only the
// settings are published and the processor remains inactive.

class SaturatorUpdate final : public AudioParamUpdate {
public:
   SaturatorSettings Settings;
   SaturatorTarget Target;
   bool Valid;

   SaturatorUpdate(const AudioParamState &State, int Rate) : Settings(saturator_settings(State)) {
      Valid = saturator_target(Settings, Rate, Target);
   }

   void publish(extAudioEffect *Effect) override {
      auto Self = (extAudioSaturator *)Effect;
      if (Self->Processor) Self->Processor->update(Settings, Valid ? &Target : nullptr);
      Self->Settings = Settings;
   }
};

static std::unique_ptr<AudioParamUpdate> saturator_prepare(extAudioEffect *Effect, const AudioParamState &State,
   int Rate)
{
   return std::make_unique<SaturatorUpdate>(State, Rate);
}

//********************************************************************************************************************

static const AudioEffectSchema glSaturatorSchema = {
   .ClassName   = "AudioSaturator",
   .Version     = 1,
   .Description = "A waveshaper that adds harmonic colour through soft saturation or hard clipping.",
   .Params      = glSaturatorParams,
   .Outputs     = glSaturatorOutputs,
   .Read        = saturator_read,
   .Apply       = saturator_apply,
   .Prepare     = saturator_prepare
};

//********************************************************************************************************************

extAudioSaturator::extAudioSaturator(objMetaClass *ClassPtr, OBJECTID ObjectID) : extAudioEffect(ClassPtr, ObjectID)
{
   Schema = &glSaturatorSchema;
}

//********************************************************************************************************************
// Storage is allocated by set_processor() outside the mixer lock.

static ERR AUDIOSATURATOR_Init(extAudioSaturator *Self)
{
   if (Self->Processor) return ERR::InvalidState;
   auto chain = Self->Chain.lock();
   if (not chain) return ERR::NotInitialised;

   AudioParamState state;
   saturator_read(Self, state);
   {
      std::lock_guard mixer_lock(*chain->Mutex);
      if (validate_state(glSaturatorSchema, state, Self->OutputRate) != ERR::Okay) return ERR::InvalidValue;
   }

   auto processor = std::make_unique<SaturatorProcessor>(Self, Self->Settings);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;

   return error;
}

//********************************************************************************************************************

#include "class_audiosaturator_def.c"

//********************************************************************************************************************

static ERR add_audiosaturator_class()
{
   clAudioSaturator = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIOEFFECT),
      fl::ClassID(CLASSID::AUDIOSATURATOR),
      fl::Name("AudioSaturator"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioSaturatorActions),
      fl::Size(sizeof(extAudioSaturator)),
      fl::Path(MOD_PATH));
   return clAudioSaturator ? ERR::Okay : ERR::AddClass;
}
