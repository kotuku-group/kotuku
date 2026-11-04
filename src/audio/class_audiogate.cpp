/*********************************************************************************************************************

-CLASS-
AudioGate: A downward expander and noise gate that attenuates quiet passages.

The gate attenuates audio whose level falls below a threshold, reducing background noise, hiss and spill in the pauses
between sounds.  It does not remove noise that is present at the same time as louder material, such as hiss beneath
speech.

Create new gate objects as a child of an @Audio object or set the inherited #AudioEffect.Audio field.  Use
#AudioEffect.Channel to process one channel set, or leave it at zero to process the global mix.  Application gates
accept live changes; global gates become immutable after initialisation.

Each parameter is available as a field, such as #Threshold and #Range.  Field writes before initialisation are
range-checked and stored; after initialisation each write is validated and applied immediately.  The same parameters are
published through the inherited @AudioEffect schema under lower-case keys, where they can be changed together with
@AudioEffect.Batch():

<list type="bullet">
<li>`mode`: `expander` (`GTM::EXPANDER`) reduces the level progressively as the signal falls further below the
threshold.  `gate` (`GTM::GATE`) switches between no attenuation and the full `range`.</li>
<li>`threshold`: the detector level, in dB relative to full scale, at which the gate opens.</li>
<li>`ratio`: the expansion slope below the threshold.  At 2, every 1 dB that the signal falls below the threshold adds
1 dB of attenuation.  A ratio of 1 disables expansion.  The ratio is ignored in gate mode.</li>
<li>`range`: the maximum attenuation, in dB.  Zero disables the effect.</li>
<li>`hysteresis`: how far, in dB, the detector must fall below the threshold before the gate starts to close.</li>
<li>`attack`: the time constant, in milliseconds, with which the gate opens.</li>
<li>`hold`: the time, in milliseconds, for which the gate remains open after the detector falls below the closing
threshold.</li>
<li>`release`: the time constant, in milliseconds, with which the gate closes.</li>
</list>

The following gate silences the pauses in a voice recording on a playing channel set:

<pre>
gate = obj.new('AudioGate', { audio=audio, channel=channel, mode=GTM_GATE, threshold=-50, range=40 })
</pre>

Every field write after initialisation is a separate change.  To change several parameters in a single step, apply them
together with @AudioEffect.Batch():

<pre>
gate.mtBatch(array&lt;str&gt; { 'set mode expander', 'set ratio 2', 'set range 15' })
</pre>

For gentler noise reduction that leaves the decay of notes more natural, use the default expander mode with a ratio
between 1.5 and 3 and a range of 10 to 20 dB.

<header>Detection</header>

The detector measures the absolute value of every sample.  In stereo, the greater of the two channel magnitudes is
used and the resulting gain is applied equally to both channels, so the louder channel controls the gate and the
stereo image does not move.  Levels are measured in dBFS, where a magnitude of 1.0 is 0 dBFS, independently of the
output format.

The detector level is the greatest magnitude received during the last 25 to 28 ms.  This window covers half a cycle
of any frequency above 20 Hz, so a sustained tone is measured by its peaks and does not reopen and close the gate at
every zero crossing.  The detector level therefore rises immediately with the input, but falls between 25 and 28 ms
after the input does.

<header>Opening and Closing</header>

The gate opens as soon as the detector reaches `threshold`.  It closes once the detector has remained below
`threshold - hysteresis` for longer than `hold`.  Between the two thresholds an open gate stays open and a closed gate
stays closed, so signals that hover around the threshold do not chatter.  In total, the gate starts to close the
`hold` time plus the 25 to 28 ms detector window after the input falls below the closing threshold.

While the gate is open no attenuation is applied.  While it is closed, the target attenuation is `range` in gate
mode.  In expander mode, for a detector level `x`, threshold `T` and ratio `R`, it is `(R - 1) * (T - x)` dB, limited
to `range`.  The output level of a closed expander is therefore `T - R * (T - x)`, until the range is reached.
Hysteresis and hold apply in both modes.

<header>Timing</header>

The target attenuation is smoothed in the dB domain.  When the attenuation is falling the attack time constant is used,
otherwise the release time constant.  Each is an exponential time constant: after that duration, 63.2% of a change in
the target has been traversed.

The gate has zero lookahead and zero latency.  The attenuation starts to fall on the first sample that reaches the
threshold, so the onset of a sound that opens a closed gate rises from the closed level at the attack rate.  Short
attack times preserve transients; longer ones soften them.  The gate never produces output in response to
silence, so it has no tail and never delays the completion of a sound.

<header>Meters</header>

In addition to the standard input and output peaks for each channel, the gate publishes a `gain_reduction` meter.  It
reports the greatest attenuation, in dB, applied during the meter interval.  A reading of zero means that the gate was
fully open throughout.

<header>Live Changes</header>

Changes to every continuous parameter ramp linearly over 10 ms, so edits do not click, and the attenuation then
follows the new settings at the current attack and release rates.  A change of `mode` blends the two target curves
over the same 10 ms.  A new `hold` time applies immediately and shortens a hold that is in progress.  An edit that
arrives during a ramp starts a new ramp from the current values.

Device reactivation, leaving bypass and reaching idle place the gate in the state that a long silence produces:
closed, with the attenuation that the current settings apply to silence.  The following sound opens the gate at the
attack rate.  Any parameter ramp is discarded and the latest parameters apply without a transition.

Output rates from 8000 to 192000 Hz are supported.  Configuring the effect for any other rate fails with
`ERR::NoSupport`.

-END-

*********************************************************************************************************************/

#include "audio_gate_dsp.h"

class extAudioGate : public extAudioEffect {
public:
   GateSettings Settings;
   GateProcessor *Processor = nullptr;

   extAudioGate(objMetaClass *ClassPtr, OBJECTID ObjectID);
};

//********************************************************************************************************************
// Parameter descriptors.  Order matches the GT_ indexes.

enum { GT_MODE = 0, GT_THRESHOLD, GT_RATIO, GT_RANGE, GT_HYSTERESIS, GT_ATTACK, GT_HOLD, GT_RELEASE };

static const AudioParamOption glGateModes[] = {
   { int(GTM::EXPANDER), "expander", "Expander",
     "Attenuation increases progressively as the signal falls further below the threshold, according to the ratio." },
   { int(GTM::GATE), "gate", "Gate", "The full range of attenuation is applied whenever the gate is closed." }
};

static const int glGateModeGate[] = { int(GTM::GATE) };

static const AudioParamRule glGateRatioRules[] = { { .Key = "mode", .Values = glGateModeGate, .Inactive = true } };

static const AudioParamDesc glGateParams[] = {
   { .Key = "mode", .Label = "Mode",
     .Description = "Whether quiet passages are attenuated progressively or by the full range.",
     .Type = APT::ENUM, .Default = int(GTM::EXPANDER), .Options = glGateModes },
   { .Key = "threshold", .Label = "Threshold",
     .Description = "The detector level, relative to full scale, at which the gate opens.",
     .Unit = APU::DB, .Min = -90, .Max = 0, .Default = -45 },
   { .Key = "ratio", .Label = "Ratio",
     .Description = "The expansion slope below the threshold.  At 2, every decibel that the signal falls below the "
        "threshold adds a decibel of attenuation.  A ratio of 1 disables expansion.",
     .Unit = APU::RATIO, .Scale = APS::LOG, .Min = 1, .Max = 20, .Default = 2, .Rules = glGateRatioRules },
   { .Key = "range", .Label = "Range",
     .Description = "The maximum attenuation.  Zero disables the effect.",
     .Unit = APU::DB, .Min = 0, .Max = 90, .Default = 60 },
   { .Key = "hysteresis", .Label = "Hysteresis",
     .Description = "How far the detector level must fall below the threshold before the gate closes.",
     .Unit = APU::DB, .Min = 0, .Max = 24, .Default = 3 },
   { .Key = "attack", .Label = "Attack",
     .Description = "The time constant with which the gate opens.",
     .Unit = APU::MS, .Scale = APS::LOG, .Min = 0.1, .Max = 100, .Default = 2 },
   { .Key = "hold", .Label = "Hold",
     .Description = "The time for which the gate remains open after the level falls below the closing threshold.",
     .Unit = APU::MS, .Min = 0, .Max = 1000, .Default = 50 },
   { .Key = "release", .Label = "Release",
     .Description = "The time constant with which the gate closes.",
     .Unit = APU::MS, .Scale = APS::LOG, .Min = 5, .Max = 2000, .Default = 150 }
};

static const AudioOutputDesc glGateOutputs[] = {
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", "Maximum input sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", "Maximum output sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::OUTPUT_PEAK },
   { "gain_reduction", AudioOutputKind::SCALAR, "Gain Reduction", "Maximum attenuation applied by the gate.", "dB",
      "global", "maximum-attenuation", AudioMeterSource::GAIN_REDUCTION }
};

//********************************************************************************************************************

static GateSettings gate_settings(const AudioParamState &State)
{
   return GateSettings {
      .Mode       = GTM(int(State.Params[GT_MODE])),
      .Threshold  = State.Params[GT_THRESHOLD],
      .Ratio      = State.Params[GT_RATIO],
      .Range      = State.Params[GT_RANGE],
      .Hysteresis = State.Params[GT_HYSTERESIS],
      .Attack     = State.Params[GT_ATTACK],
      .Hold       = State.Params[GT_HOLD],
      .Release    = State.Params[GT_RELEASE]
   };
}

static void gate_read(extAudioEffect *Effect, AudioParamState &State)
{
   const auto &settings = ((extAudioGate *)Effect)->Settings;
   State.Params.assign({ double(int(settings.Mode)), settings.Threshold, settings.Ratio, settings.Range,
      settings.Hysteresis, settings.Attack, settings.Hold, settings.Release });
   State.Groups.clear();
}

//********************************************************************************************************************
// Before initialisation there is no processor.

static void gate_apply(extAudioEffect *Effect, const AudioParamState &State)
{
   ((extAudioGate *)Effect)->Settings = gate_settings(State);
}

//********************************************************************************************************************
// The target is derived outside the mixer lock.  An unsupported rate leaves the target invalid, in which case only
// the settings are published and the processor remains inactive.

class GateUpdate final : public AudioParamUpdate {
public:
   GateSettings Settings;
   GateTarget Target;
   bool Valid;

   GateUpdate(const AudioParamState &State, int Rate) : Settings(gate_settings(State)) {
      Valid = gate_target(Settings, Rate, Target);
   }

   void publish(extAudioEffect *Effect) override {
      auto Self = (extAudioGate *)Effect;
      if (Self->Processor) Self->Processor->update(Settings, Valid ? &Target : nullptr);
      Self->Settings = Settings;
   }
};

static std::unique_ptr<AudioParamUpdate> gate_prepare(extAudioEffect *Effect, const AudioParamState &State, int Rate)
{
   return std::make_unique<GateUpdate>(State, Rate);
}

//********************************************************************************************************************

static const AudioEffectSchema glGateSchema = {
   .ClassName   = "AudioGate",
   .Version     = 1,
   .Description = "A downward expander and noise gate that attenuates quiet passages.",
   .Params      = glGateParams,
   .Outputs     = glGateOutputs,
   .Read        = gate_read,
   .Apply       = gate_apply,
   .Prepare     = gate_prepare
};

//********************************************************************************************************************

extAudioGate::extAudioGate(objMetaClass *ClassPtr, OBJECTID ObjectID) : extAudioEffect(ClassPtr, ObjectID)
{
   Schema = &glGateSchema;
}

//********************************************************************************************************************

static ERR AUDIOGATE_Init(extAudioGate *Self)
{
   if (Self->Processor) return ERR::InvalidState;
   auto chain = Self->Chain.lock();
   if (not chain) return ERR::NotInitialised;

   AudioParamState state;
   gate_read(Self, state);
   {
      std::lock_guard mixer_lock(*chain->Mutex);
      if (validate_state(glGateSchema, state, Self->OutputRate) != ERR::Okay) return ERR::InvalidValue;
   }

   auto processor = std::make_unique<GateProcessor>(Self, Self->Settings);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;

   return error;
}

/*********************************************************************************************************************

-FIELD-
Attack: The time constant, in milliseconds, with which the gate opens.

The range is 0.1 to 100 ms and the default is 2 ms.  This field mirrors the `attack` schema key.  To change it together
with other parameters in a single step, use @AudioEffect.Batch().

-FIELD-
Hold: The time, in milliseconds, for which the gate remains open after the level falls below the closing threshold.

The range is 0 to 1000 ms and the default is 50 ms.  This field mirrors the `hold` schema key.  To change it together
with other parameters in a single step, use @AudioEffect.Batch().

-FIELD-
Hysteresis: How far, in decibels, the detector level must fall below the threshold before the gate closes.

The range is 0 to 24 dB and the default is 3 dB.  This field mirrors the `hysteresis` schema key.  To change it together
with other parameters in a single step, use @AudioEffect.Batch().

-FIELD-
Mode: Selects progressive or full attenuation of quiet passages.
Lookup: GTM

The default is `GTM::EXPANDER`.  This field mirrors the `mode` schema key, which also accepts the option names
`expander` and `gate`.  To change it together with other parameters in a single step, use @AudioEffect.Batch().

-FIELD-
Range: The maximum attenuation, in decibels.

The range is 0 to 90 dB and the default is 60 dB.  Zero disables the effect.  This field mirrors the `range` schema key.
To change it together with other parameters in a single step, use @AudioEffect.Batch().

-FIELD-
Ratio: The expansion slope below the threshold.

The range is 1 to 20 and the default is 2.  At 2, every decibel that the signal falls below the threshold adds a decibel
of attenuation.  A ratio of 1 disables expansion.  The ratio is ignored when #Mode is `GTM::GATE`.  This field mirrors
the `ratio` schema key.  To change it together with other parameters in a single step, use @AudioEffect.Batch().

-FIELD-
Release: The time constant, in milliseconds, with which the gate closes.

The range is 5 to 2000 ms and the default is 150 ms.  This field mirrors the `release` schema key.  To change it
together with other parameters in a single step, use @AudioEffect.Batch().

-FIELD-
Threshold: The detector level, in dBFS, at which the gate opens.

The range is -90 to 0 dBFS and the default is -45 dBFS.  This field mirrors the `threshold` schema key.  To change it
together with other parameters in a single step, use @AudioEffect.Batch().

*********************************************************************************************************************/

#include "class_audiogate_def.c"

static const FieldArray clAudioGateFields[] = {
   { "Mode",       FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW|FDF_PURE,
     effect_settings_get_int<extAudioGate, &GateSettings::Mode>, effect_param_set_int<GT_MODE>, &clAudioGateGTM },
   { "Threshold",  FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE,
     effect_settings_get<extAudioGate, &GateSettings::Threshold>, effect_param_set<GT_THRESHOLD> },
   { "Ratio",      FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE,
     effect_settings_get<extAudioGate, &GateSettings::Ratio>, effect_param_set<GT_RATIO> },
   { "Range",      FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE,
     effect_settings_get<extAudioGate, &GateSettings::Range>, effect_param_set<GT_RANGE> },
   { "Hysteresis", FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE,
     effect_settings_get<extAudioGate, &GateSettings::Hysteresis>, effect_param_set<GT_HYSTERESIS> },
   { "Attack",     FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE,
     effect_settings_get<extAudioGate, &GateSettings::Attack>, effect_param_set<GT_ATTACK> },
   { "Hold",       FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE,
     effect_settings_get<extAudioGate, &GateSettings::Hold>, effect_param_set<GT_HOLD> },
   { "Release",    FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE,
     effect_settings_get<extAudioGate, &GateSettings::Release>, effect_param_set<GT_RELEASE> },
   END_FIELD
};

//********************************************************************************************************************

static ERR add_audiogate_class()
{
   clAudioGate = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIOEFFECT),
      fl::ClassID(CLASSID::AUDIOGATE),
      fl::Name("AudioGate"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioGateActions),
      fl::Fields(clAudioGateFields),
      fl::Size(sizeof(extAudioGate)),
      fl::Path(MOD_PATH));
   return clAudioGate ? ERR::Okay : ERR::AddClass;
}
