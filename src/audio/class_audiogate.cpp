/*********************************************************************************************************************

-CLASS-
AudioGate: A downward expander and noise gate that attenuates quiet passages.

The gate attenuates audio whose level falls below a threshold, reducing background noise, hiss and spill in the pauses
between sounds.  It does not remove noise that is present at the same time as louder material, such as hiss beneath
speech.

Create new gate objects as a child of an @Audio object or set the inherited #AudioEffect.Audio field.  Use
#AudioEffect.Channel to process one channel set, or leave it at zero to process the global mix.  Application gates
accept live changes; global gates become immutable after initialisation.

Parameters are published through the inherited @AudioEffect schema and are changed with @AudioEffect.SetKey()
followed by @AudioEffect.Flush():

<list type="bullet">
<li>`mode`: `expander` (0) reduces the level progressively as the signal falls further below the threshold.  `gate`
(1) switches between no attenuation and the full `range`.</li>
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
gate = obj.new('AudioGate', { audio=audio, channel=channel })
gate.acSetKey('mode', 1)
gate.acSetKey('threshold', -50)
gate.acSetKey('range', 40)
gate.acFlush()
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
   { GATE_EXPANDER, "expander", "Expander",
     "Attenuation increases progressively as the signal falls further below the threshold, according to the ratio." },
   { GATE_GATE, "gate", "Gate", "The full range of attenuation is applied whenever the gate is closed." }
};

static const int glGateModeGate[] = { GATE_GATE };

static const AudioParamRule glGateRatioRules[] = { { .Key = "mode", .Values = glGateModeGate, .Inactive = true } };

static const AudioParamDesc glGateParams[] = {
   { .Key = "mode", .Label = "Mode",
     .Description = "Whether quiet passages are attenuated progressively or by the full range.",
     .Type = APT::ENUM, .Default = GATE_EXPANDER, .Options = glGateModes },
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
      .Mode       = int(State.Params[GT_MODE]),
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
   State.Params.assign({ double(settings.Mode), settings.Threshold, settings.Ratio, settings.Range,
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

//********************************************************************************************************************

#include "class_audiogate_def.c"

static const FieldArray clAudioGateFields[] = {
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
