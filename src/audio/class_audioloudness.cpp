/*********************************************************************************************************************

-CLASS-
AudioLoudness: Adapts playback gain towards a target loudness.

The loudness leveller measures the perceived loudness of the audio passing through it and adjusts its gain so that
material from different sources plays at a consistent level.  A quiet recording is raised and a loud one lowered, each
within a configurable limit, and the gain changes slowly enough that the dynamics within a recording are preserved.

Create new leveller objects as a child of an @Audio object or set the inherited #AudioEffect.Audio field.  Use
#AudioEffect.Channel to process one channel set, or leave it at zero to process the global mix.  Application
levellers accept live changes; global levellers become immutable after initialisation.

Parameters are published through the inherited @AudioEffect schema and are changed with @AudioEffect.SetKey()
followed by @AudioEffect.Flush():

<list type="bullet">
<li>`target`: the short-term loudness, in LUFS, that the leveller aims for.  Common references are -14 LUFS for music
streaming, -16 LUFS for podcasts and spoken word, and -23 LUFS for broadcast programmes.</li>
<li>`max_boost`: the maximum gain, in dB, applied to quiet material.  Zero prevents any boost.</li>
<li>`max_cut`: the maximum attenuation, in dB, applied to loud material.  Zero prevents any cut.</li>
<li>`response`: the time constant, in milliseconds, with which the gain moves towards its target.</li>
<li>`gate`: the loudness, in LUFS, below which the gain is held rather than adjusted.</li>
<li>`link`: when enabled (the default), one gain is applied to every channel.  When disabled, every channel is
levelled independently.</li>
</list>

The following leveller brings a channel set towards the -16 LUFS spoken-word reference, boosting quiet material by up
to 9 dB:

<pre>
leveller = obj.new('AudioLoudness', { audio=audio, channel=channel })
leveller.acSetKey('target', -16)
leveller.acSetKey('max_boost', 9)
leveller.acFlush()
</pre>

The leveller does not limit peaks.  A boost can raise peaks above full scale, so place an @AudioLimiter after the
leveller, with a higher #AudioEffect.Order, whenever `max_boost` is not zero.

<header>Measurement</header>

Loudness is measured as defined by ITU-R BS.1770.  Each channel is K-weighted by a high-frequency shelf and a high-pass
filter, both designed for the output rate, and the mean square of the result is accumulated in 100 ms blocks.
Momentary loudness is measured over the last 400 ms and short-term loudness over the last 3 s.  Measurements are
updated every 100 ms.  After a reset, until a window has filled, it covers the audio received since the reset.

Every channel is weighted equally.  BS.1770 measures a mono layout as a single channel, so mono material played on
two speakers is perceived as louder than its measurement.

The input of the leveller is measured, not its output, so the gain never influences its own measurement.  This is
real-time levelling and is not a measurement of integrated programme loudness or a substitute for ReplayGain-style
analysis of a complete file.

<header>Gain</header>

The target gain is the difference between `target` and the short-term loudness, limited to the range from `-max_cut`
to `max_boost`.  The applied gain follows the target in decibels with an exponential time constant of `response`:
after that time, 63.2% of a change in the target has been traversed.  Since short-term loudness itself spans 3 s, the
gain responds to a sudden change in level over the window length plus a few response time constants.

While either the momentary or the short-term loudness is below `gate`, the gain is held at its current value, so that
silence and background noise between sounds are not raised.  Holding on the momentary loudness stops the gain from
rising during the 3 s in which the short-term window empties after a sound ends.  The held gain is still limited by
`max_boost` and `max_cut`.  Set `gate` above the noise floor of the material, otherwise noise in long pauses is
raised by up to `max_boost`.

<header>Linked and Unlinked Operation</header>

Linked operation measures the loudness of the programme across all channels and applies one gain to every channel,
which preserves the stereo image.

Unlinked operation measures and levels every channel on its own.  Each channel's loudness is scaled to its share of the
programme, so balanced material receives the same gain in both modes.  This evens out recordings with one channel
quieter than the other, but the stereo image moves as the channel gains diverge.  Changing `link` moves every channel
from its current gain to its new target at the `response` rate.

<header>Meters</header>

In addition to the standard input and output peaks for each channel, the leveller publishes:

<list type="bullet">
<li>`momentary_loudness`: the momentary loudness of the input programme, in LUFS, floored at -120 LUFS.</li>
<li>`short_term_loudness`: the short-term loudness of the input programme, in LUFS, floored at -120 LUFS.</li>
<li>`applied_gain_left`, `applied_gain_right` or `applied_gain_centre`: the signed gain, in dB, applied to the
channel at the end of the meter interval.  Positive values are boosts.  In linked operation every channel reports the
same gain.</li>
</list>

The loudness meters are measurements of the programme in both linked and unlinked operation.

<header>Live Changes</header>

Parameter changes take effect immediately, and the applied gain moves to the new target at the `response` rate, so
edits never click.  The measurement history is retained.

Reaching idle discards the measurement history but preserves the applied gain, so adjacent sounds start at a
consistent level.  Device reactivation and leaving bypass discard the history and return the leveller to unity gain.
The leveller has zero latency and no tail, and never delays the completion of a sound.

Output rates from 8000 to 192000 Hz are supported.  Configuring the effect for any other rate fails with
`ERR::NoSupport`.

-END-

*********************************************************************************************************************/

#include "audio_loudness_dsp.h"

class extAudioLoudness : public extAudioEffect {
public:
   LoudnessSettings Settings;
   LoudnessProcessor *Processor = nullptr;

   extAudioLoudness(objMetaClass *ClassPtr, OBJECTID ObjectID);
};

//********************************************************************************************************************
// Parameter descriptors.  Order matches the LD_ indexes.

enum { LD_TARGET = 0, LD_MAX_BOOST, LD_MAX_CUT, LD_RESPONSE, LD_GATE, LD_LINK };

static const AudioParamDesc glLoudnessParams[] = {
   { .Key = "target", .Label = "Target",
     .Description = "The short-term loudness, in LUFS, that the leveller aims for.  Common references are -14 for "
        "music streaming, -16 for podcasts and spoken word, and -23 for broadcast programmes.",
     .Unit = APU::DB, .Min = -36, .Max = -6, .Default = -16 },
   { .Key = "max_boost", .Label = "Max Boost",
     .Description = "The maximum gain applied to quiet material.",
     .Unit = APU::DB, .Min = 0, .Max = 24, .Default = 12 },
   { .Key = "max_cut", .Label = "Max Cut",
     .Description = "The maximum attenuation applied to loud material.",
     .Unit = APU::DB, .Min = 0, .Max = 24, .Default = 12 },
   { .Key = "response", .Label = "Response",
     .Description = "The time constant with which the gain moves towards its target.",
     .Unit = APU::MS, .Scale = APS::LOG, .Min = 500, .Max = 30000, .Default = 3000 },
   { .Key = "gate", .Label = "Gate",
     .Description = "The loudness, in LUFS, below which the gain is held rather than adjusted, so that silence and "
        "background noise are not raised.",
     .Unit = APU::DB, .Min = -90, .Max = -30, .Default = -70 },
   { .Key = "link", .Label = "Link",
     .Description = "Apply one gain to every channel.  When off, each channel is levelled independently, which can "
        "move the stereo image.",
     .Type = APT::BOOL, .Min = 0, .Max = 1, .Default = 1 }
};

static const AudioOutputDesc glLoudnessOutputs[] = {
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", "Maximum input sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", "Maximum output sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::OUTPUT_PEAK },
   { "momentary_loudness", AudioOutputKind::SCALAR, "Momentary Loudness",
      "Loudness of the input programme over the last 400 ms.", "LUFS", "global", "momentary-loudness",
      AudioMeterSource::PROCESSOR, LOUDNESS_MOMENTARY, LOUDNESS_FLOOR },
   { "short_term_loudness", AudioOutputKind::SCALAR, "Short-Term Loudness",
      "Loudness of the input programme over the last 3 seconds.", "LUFS", "global", "short-term-loudness",
      AudioMeterSource::PROCESSOR, LOUDNESS_SHORT_TERM, LOUDNESS_FLOOR },
   { "applied_gain", AudioOutputKind::SCALAR, "Applied Gain",
      "Signed gain applied at the end of the interval; positive values are boosts.", "dB", "channel", "applied-gain",
      AudioMeterSource::PROCESSOR, LOUDNESS_APPLIED_GAIN, 0 }
};

//********************************************************************************************************************

static LoudnessSettings loudness_settings(const AudioParamState &State)
{
   return LoudnessSettings {
      .Target   = State.Params[LD_TARGET],
      .MaxBoost = State.Params[LD_MAX_BOOST],
      .MaxCut   = State.Params[LD_MAX_CUT],
      .Response = State.Params[LD_RESPONSE],
      .Gate     = State.Params[LD_GATE],
      .Link     = State.Params[LD_LINK] != 0
   };
}

static void loudness_read(extAudioEffect *Effect, AudioParamState &State)
{
   const auto &settings = ((extAudioLoudness *)Effect)->Settings;
   State.Params.assign({ settings.Target, settings.MaxBoost, settings.MaxCut, settings.Response, settings.Gate,
      settings.Link ? 1.0 : 0.0 });
   State.Groups.clear();
}

//********************************************************************************************************************
// Before initialisation there is no processor.

static void loudness_apply(extAudioEffect *Effect, const AudioParamState &State)
{
   ((extAudioLoudness *)Effect)->Settings = loudness_settings(State);
}

//********************************************************************************************************************
// The target is derived outside the mixer lock.  An unsupported rate leaves the target invalid, in which case only
// the settings are published and the processor remains inactive.

class LoudnessUpdate final : public AudioParamUpdate {
public:
   LoudnessSettings Settings;
   LoudnessTarget Target;
   bool Valid;

   LoudnessUpdate(const AudioParamState &State, int Rate) : Settings(loudness_settings(State)) {
      Valid = loudness_target(Settings, Rate, Target);
   }

   void publish(extAudioEffect *Effect) override {
      auto Self = (extAudioLoudness *)Effect;
      if (Self->Processor) Self->Processor->update(Settings, Valid ? &Target : nullptr);
      Self->Settings = Settings;
   }
};

static std::unique_ptr<AudioParamUpdate> loudness_prepare(extAudioEffect *Effect, const AudioParamState &State,
   int Rate)
{
   return std::make_unique<LoudnessUpdate>(State, Rate);
}

//********************************************************************************************************************

static const AudioEffectSchema glLoudnessSchema = {
   .ClassName   = "AudioLoudness",
   .Version     = 1,
   .Description = "Adapts playback gain towards a target loudness, so that material from different sources plays at "
      "a consistent perceived level.",
   .Params      = glLoudnessParams,
   .Outputs     = glLoudnessOutputs,
   .Read        = loudness_read,
   .Apply       = loudness_apply,
   .Prepare     = loudness_prepare
};

//********************************************************************************************************************

extAudioLoudness::extAudioLoudness(objMetaClass *ClassPtr, OBJECTID ObjectID) : extAudioEffect(ClassPtr, ObjectID)
{
   Schema = &glLoudnessSchema;
}

//********************************************************************************************************************

static ERR AUDIOLOUDNESS_Init(extAudioLoudness *Self)
{
   if (Self->Processor) return ERR::InvalidState;
   auto chain = Self->Chain.lock();
   if (not chain) return ERR::NotInitialised;

   AudioParamState state;
   loudness_read(Self, state);
   {
      std::lock_guard mixer_lock(*chain->Mutex);
      if (validate_state(glLoudnessSchema, state, Self->OutputRate) != ERR::Okay) return ERR::InvalidValue;
   }

   auto processor = std::make_unique<LoudnessProcessor>(Self, Self->Settings);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;

   return error;
}

//********************************************************************************************************************

#include "class_audioloudness_def.c"

static const FieldArray clAudioLoudnessFields[] = {
   END_FIELD
};

//********************************************************************************************************************

static ERR add_audioloudness_class()
{
   clAudioLoudness = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIOEFFECT),
      fl::ClassID(CLASSID::AUDIOLOUDNESS),
      fl::Name("AudioLoudness"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioLoudnessActions),
      fl::Fields(clAudioLoudnessFields),
      fl::Size(sizeof(extAudioLoudness)),
      fl::Path(MOD_PATH));
   return clAudioLoudness ? ERR::Okay : ERR::AddClass;
}
