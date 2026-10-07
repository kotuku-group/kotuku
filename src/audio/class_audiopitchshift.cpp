/*********************************************************************************************************************

-CLASS-
AudioPitchShift: Transposes audio by up to an octave in either direction without changing its speed.

The pitch shifter raises or lowers the frequencies of its input by a fixed musical interval while the audio continues to
play at its original speed and duration.  Typical uses include transposing music into another key, creative vocal
effects and adding a shifted copy of a sound for thickening or harmony.

Create new pitch shift objects as a child of an @Audio object or set the inherited #AudioEffect.Audio field.  Use
#AudioEffect.Channel to process one channel set, or leave it at zero to process the global mix.  Application effects
accept live changes; global effects become immutable after initialisation.

Parameters are published through the inherited @AudioEffect schema and are changed with @AudioEffect.SetKey()
followed by @AudioEffect.Flush():

<list type="bullet">
<li>`semitones`: the pitch offset, from -12 to 12 semitones.  Fractional values are accepted, so 0.1 detunes by ten
cents.  Zero leaves the pitch unchanged.</li>
<li>`mix`: the linear blend of the dry and shifted signals, from 0 to 100 percent.  100 percent outputs only the
shifted signal.</li>
<li>`gain`: the output trim, from -24 to 12 dB, applied to the blended signal.</li>
</list>

The following adds a fifth above a playing channel set, blended equally with the original, as a single change:

<pre>
shifter = obj.new('AudioPitchShift', { audio=audio, channel=channel })
shifter.acSetKey('semitones', 7)
shifter.acSetKey('mix', 50)
shifter.acFlush()
</pre>

Other useful combinations include a one-octave drop with `semitones` -12 and `mix` 100, and a light doubling effect
with `semitones` 0.15 and `mix` 50.

<header>Processing</header>

The shifter is a phase vocoder.  It analyses overlapping windows of the input into their frequency components, moves
each spectral peak and the bins around it to the transposed frequency, and resynthesises the result.  Each window spans
32 to 47 ms depending on the output rate (2048 frames at 44.1 and 48 kHz) and a new window starts every eighth of
that.  The partials of a sustained sound leave at exactly the transposed frequency.

Every frequency moves by the same ratio, so the spectral envelope moves with the pitch: voices raised by several
semitones take on a smaller, brighter character and lowered voices sound larger.  Formants are not preserved.  This is
transposition, not automatic pitch correction, and the interval is not adjusted to follow the input.

Transients are the main limitation.  Each window is processed as a whole, so the onset of a sharp sound such as a drum
hit or a consonant is spread across the window.  Most of its energy stays within 5 ms of the original timing, but a
faint pre-echo can precede it by up to the window length and percussive material softens noticeably at large shifts.
Sustained tones, chords and most music shift cleanly.  Raising the pitch discards content that would move above the
Nyquist frequency, so upward shifts narrow the bandwidth by the same ratio.

Stereo channels are analysed together and receive identical processing, so the level and phase differences between
them, and with them the stereo image, are preserved.  Mono output is shifted in the same way.

<header>Latency</header>

The shifted signal is delayed by exactly one window: 2048 frames (42.7 ms) at 48 kHz, 46.4 ms at 44.1 kHz and between
32 and 47 ms at other rates.  The pitch shifter publishes this as its #AudioEffect.Latency.  The dry signal is delayed
by the same amount so that the two are time-aligned at every `mix` setting.  The latency is the same at every setting,
including zero semitones and zero mix.

At zero semitones the shifted signal reproduces the input exactly, apart from rounding, so any `mix` setting gives the
delayed input.  After a shift returns to zero, the components of the shifted signal take up to two windows to realign
with the dry signal, during which partials are detuned by no more than 6 Hz at 48 kHz.

<header>Levels and Headroom</header>

Levels are measured in dBFS, where a sample magnitude of 1.0 is 0 dBFS, independently of the output format.  Sustained
tones keep their level within 0.05 dB at every shift.  Noise-like content, such as cymbals, breath and distorted
guitars, loses between 0.5 dB and 1.4 dB, more for larger shifts, so dense material can play slightly quieter: a
heavily distorted rock recording measured 0.3 to 0.9 dB quieter and an orchestral recording within 0.15 dB.  Raise
`gain` to compensate if required.

The partials of a complex sound leave with new relative phases, so the peaks of the shifted waveform can be higher
than those of the input at the same loudness, by 4 dB or more on heavily compressed recordings.  Place an
@AudioLimiter after the shifter when a sample ceiling is required.  Non-finite samples are outside the normalised mixing
contract and are treated as silence.

<header>Live Changes</header>

A change to `semitones` applies from the next analysis window, within 5.3 ms at 48 kHz, and the new pitch blends in
over the overlapping windows.  Changes to `mix` and `gain` ramp linearly over 10 ms from their current values.  Flush
preserves the analysis history and the dry delay.

Device reactivation, rate or layout changes and leaving bypass discard the history and apply the latest parameters
without a transition.  Hard bypass removes the latency along with the processing.

<header>Tail and Drain</header>

After its input stops, the shifter outputs one latency period of buffered audio followed by the remainder of the last
windows: up to two window lengths in total.  The effect then stops reporting pending output.  Hard bypass and
destruction cut the tail abruptly.

Output rates from 8000 to 192000 Hz are supported.  Configuring the effect for any other rate fails with
`ERR::NoSupport`.  Storage is allocated when the output is configured and is about 300 KB for stereo at 48 kHz.

-END-

*********************************************************************************************************************/

#include "audio_pitchshift_dsp.h"

class extAudioPitchShift : public extAudioEffect {
public:
   PitchShiftSettings Settings;
   PitchShiftProcessor *Processor = nullptr;

   extAudioPitchShift(objMetaClass *ClassPtr, OBJECTID ObjectID);
};

//********************************************************************************************************************
// Parameter descriptors.  Order matches the PS_ indexes.

enum { PS_SEMITONES = 0, PS_MIX, PS_GAIN };

static const AudioParamDesc glPitchShiftParams[] = {
   { .Key = "semitones", .Label = "Semitones",
     .Description = "The pitch offset in semitones.  Fractional values detune by cents; zero leaves the pitch "
        "unchanged.",
     .Min = -PITCH_MAX_SHIFT, .Max = PITCH_MAX_SHIFT, .Default = 0 },
   { .Key = "mix", .Label = "Mix",
     .Description = "The linear blend with the latency-aligned dry input.  100 percent outputs only the shifted "
        "signal.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 100, .Default = 100 },
   { .Key = "gain", .Label = "Gain",
     .Description = "The output trim.",
     .Unit = APU::DB, .Min = -24, .Max = 12, .Default = 0 }
};

static const AudioOutputDesc glPitchShiftOutputs[] = {
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", "Maximum input sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", "Maximum output sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::OUTPUT_PEAK }
};

//********************************************************************************************************************

static PitchShiftSettings pitchshift_settings(const AudioParamState &State)
{
   return PitchShiftSettings {
      .Semitones = State.Params[PS_SEMITONES],
      .Mix       = State.Params[PS_MIX],
      .Gain      = State.Params[PS_GAIN]
   };
}

static void pitchshift_read(extAudioEffect *Effect, AudioParamState &State)
{
   const auto &settings = ((extAudioPitchShift *)Effect)->Settings;
   State.Params.assign({ settings.Semitones, settings.Mix, settings.Gain });
   State.Groups.clear();
}

//********************************************************************************************************************
// Before initialisation there is no processor.

static void pitchshift_apply(extAudioEffect *Effect, const AudioParamState &State)
{
   ((extAudioPitchShift *)Effect)->Settings = pitchshift_settings(State);
}

//********************************************************************************************************************
// Targets are derived outside the mixer lock.  An unsupported rate leaves the target invalid, in which case only the
// settings are published and the processor remains inactive.

class PitchShiftUpdate final : public AudioParamUpdate {
public:
   PitchShiftSettings Settings;
   PitchShiftTarget Target;
   bool Valid;

   PitchShiftUpdate(const AudioParamState &State, int Rate) : Settings(pitchshift_settings(State)) {
      Valid = pitch_target(Settings, Rate, Target);
   }

   void publish(extAudioEffect *Effect) override {
      auto Self = (extAudioPitchShift *)Effect;
      if (Self->Processor) Self->Processor->update(Settings, Valid ? &Target : nullptr);
      Self->Settings = Settings;
   }
};

static std::unique_ptr<AudioParamUpdate> pitchshift_prepare(extAudioEffect *Effect, const AudioParamState &State,
   int Rate)
{
   return std::make_unique<PitchShiftUpdate>(State, Rate);
}

//********************************************************************************************************************

static const AudioEffectSchema glPitchShiftSchema = {
   .ClassName   = "AudioPitchShift",
   .Version     = 1,
   .Description = "Transposes audio by up to an octave in either direction without changing its speed.",
   .Params      = glPitchShiftParams,
   .Outputs     = glPitchShiftOutputs,
   .Read        = pitchshift_read,
   .Apply       = pitchshift_apply,
   .Prepare     = pitchshift_prepare
};

//********************************************************************************************************************

extAudioPitchShift::extAudioPitchShift(objMetaClass *ClassPtr, OBJECTID ObjectID) : extAudioEffect(ClassPtr, ObjectID)
{
   Schema = &glPitchShiftSchema;
}

//********************************************************************************************************************

static ERR AUDIOPITCHSHIFT_Init(extAudioPitchShift *Self)
{
   if (Self->Processor) return ERR::InvalidState;
   auto chain = Self->Chain.lock();
   if (not chain) return ERR::NotInitialised;

   AudioParamState state;
   pitchshift_read(Self, state);
   {
      std::lock_guard mixer_lock(*chain->Mutex);
      if (validate_state(glPitchShiftSchema, state, Self->OutputRate) != ERR::Okay) return ERR::InvalidValue;
   }

   auto processor = std::make_unique<PitchShiftProcessor>(Self, Self->Settings);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;

   return error;
}

//********************************************************************************************************************

#include "class_audiopitchshift_def.c"

//********************************************************************************************************************

static ERR add_audiopitchshift_class()
{
   clAudioPitchShift = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIOEFFECT),
      fl::ClassID(CLASSID::AUDIOPITCHSHIFT),
      fl::Name("AudioPitchShift"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioPitchShiftActions),
      fl::Size(sizeof(extAudioPitchShift)),
      fl::Path(MOD_PATH));
   return clAudioPitchShift ? ERR::Okay : ERR::AddClass;
}
