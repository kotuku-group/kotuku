/*********************************************************************************************************************

-CLASS-
AudioStereo: Adjusts the stereo width, balance and channel order of the sound.

The stereo effect separates its input into a mid signal, which is the part common to both channels, and a side signal,
which is the difference between them.  Scaling the side signal narrows or widens the stereo image without changing
sounds at the centre: zero width collapses the image to mono, 100 percent leaves it unchanged and 200 percent doubles
the difference between the channels.  Balance then attenuates one channel, and a final trim sets the output level.

The width can optionally be split at a crossover frequency, so that bass and treble receive different widths.  The
most common use is mono bass, which keeps low frequencies centred for speakers, subwoofers and vinyl mastering while
the upper frequencies keep their width.  The opposite arrangement widens only the upper frequencies, giving a broader
image without a diffuse bass.

Create new stereo objects as a child of an @Audio object or set the inherited #AudioEffect.Audio field.  Use
#AudioEffect.Channel to process one channel set, or leave it at zero to process the global mix.  Application effects
accept live changes; global effects become immutable after initialisation.

Parameters are published through the inherited @AudioEffect schema and are changed with @AudioEffect.SetKey()
followed by @AudioEffect.Flush():

<list type="bullet">
<li>`width`: the side level, from 0 to 200 percent.  Zero gives mono output and 100 leaves the image unchanged.</li>
<li>`balance`: from -100 to 100 percent.  Negative values attenuate the right channel and positive values the left,
linearly, so that the far ends silence that channel.</li>
<li>`swap`: when enabled, exchanges the left and right channels before the width is applied.</li>
<li>`split`: when enabled, applies `bass_width` instead of `width` below the crossover frequency.</li>
<li>`crossover`: the frequency below which `bass_width` applies, from 40 to 1000 Hz.  Ignored unless `split` is
enabled.</li>
<li>`bass_width`: the side level below the crossover, from 0 to 200 percent.  Zero gives mono bass.  Ignored unless
`split` is enabled.</li>
<li>`gain`: the output trim, from -24 to 12 dB.</li>
</list>

The following centres everything below 120 Hz while leaving the rest of the image unchanged, as a single change:

<pre>
stereo = obj.new('AudioStereo', { audio=audio, channel=channel })
stereo.acSetKey('split', 1)
stereo.acSetKey('crossover', 120)
stereo.acSetKey('bass_width', 0)
stereo.acFlush()
</pre>

Other useful combinations include a wider image with `width` 140; a mono check with `width` 0; a broader image with
an unchanged bass, by enabling `split` with `width` 150 and `bass_width` 100; and correcting reversed wiring with
`swap`.

<header>Processing</header>

For left and right inputs `L` and `R`, the mid signal is `M = (L + R) / 2` and the side signal is `S = (L - R) / 2`.
The outputs are `M + w * S` and `M - w * S`, where `w` is the width divided by 100, followed by the balance and the
trim.  At the default settings the output is identical to the input.

With `split` enabled, the side signal is divided into `S_high`, a second-order Butterworth high-pass of `S` at the
crossover, and `S_low = S - S_high`.  The processed side signal is `b * S_low + w * S_high`, where `b` is the bass
width divided by 100.  Because `S_low` is the exact complement of `S_high`, equal widths give exactly the unsplit
result.  The high band falls by 12 dB per octave below the crossover, so with a bass width of zero the remaining side
level is -3 dB at the crossover, -12 dB an octave below it and -28 dB at a fifth of it, with no boost at any
frequency.  The complementary low band falls by only 6 dB per octave above the crossover and is still at -9 dB two
octaves above it, so a non-zero bass width that differs from the width shades the side level well above the
crossover.  Only the side signal is filtered, so the mid
signal, and with it any sound in the centre of the image, is never phase-shifted.

A mono output has no stereo image.  Width, balance, swap and split have no effect and only the trim is applied.

<header>Levels and Headroom</header>

Levels are measured in dBFS, where a sample magnitude of 1.0 is 0 dBFS, independently of the output format.  A width
above 100 percent amplifies the side signal and can raise peaks above those of the input: a signal present in only
one channel peaks 50 percent higher at the maximum width.  Wide settings also deepen the cancellation heard when the
output is summed to mono, because the side signal is removed by the sum while the listener has grown used to it.  The
effect does not normalise these changes away; reduce `gain` or follow the effect with a limiter if required.  Balance
only attenuates.  Non-finite samples are outside the normalised mixing contract and are treated as silence.

<header>Live Changes</header>

Changes to `width`, `bass_width`, `balance`, `gain` and `split` ramp linearly over 10 ms from their current values.
Enabling or disabling `split` leaves content well above the crossover at its width throughout the ramp.
A change to `swap` ramps the side signal through zero to its inverse over 10 ms, so the image passes briefly through
mono rather than jumping.  A change to `crossover` crossfades from the old filter to the new one over 10 ms; further
changes during a crossfade take effect when it ends.  Flush preserves the filter history.

Device reactivation, rate or layout changes and leaving bypass discard the filter history and apply the latest
parameters without a transition.

<header>Tail and Drain</header>

The effect has zero latency.  Without `split`, or with equal widths, each output frame depends only on the input
frame, and the effect has no tail.  Otherwise the crossover filter rings briefly after the side signal stops: the
effect remains pending until the filter history is 180 dB below the loudest side input, which takes 20 to 30 ms at
the default crossover and up to 100 ms at the lowest crossover.  The side signal of mono content is zero, so it never
excites the filter.  Hard bypass and destruction
cut the tail abruptly.

Output rates from 8000 to 192000 Hz are supported.  Configuring the effect for any other rate fails with
`ERR::NoSupport`.  The effect allocates no audio memory.

-END-

*********************************************************************************************************************/

#include "audio_stereo_dsp.h"

class extAudioStereo : public extAudioEffect {
public:
   StereoSettings Settings;
   StereoProcessor *Processor = nullptr;

   extAudioStereo(objMetaClass *ClassPtr, OBJECTID ObjectID);
};

//********************************************************************************************************************
// Parameter descriptors.  Order matches the ST_ indexes.

enum { ST_WIDTH = 0, ST_BALANCE, ST_SWAP, ST_SPLIT, ST_CROSSOVER, ST_BASS_WIDTH, ST_GAIN };

static const int glStereoSplitOff[] = { 0 };

static const AudioParamRule glStereoSplitRules[] = {
   { .Key = "split", .Values = glStereoSplitOff, .Inactive = true }
};

static const AudioParamDesc glStereoParams[] = {
   { .Key = "width", .Label = "Width",
     .Description = "The level of the difference between the channels.  Zero gives mono and 100% leaves the image "
        "unchanged.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 200, .Default = 100 },
   { .Key = "balance", .Label = "Balance",
     .Description = "Negative values attenuate the right channel and positive values attenuate the left.",
     .Unit = APU::PERCENT, .Min = -100, .Max = 100, .Default = 0 },
   { .Key = "swap", .Label = "Swap",
     .Description = "Exchanges the left and right channels before the width is applied.",
     .Type = APT::BOOL, .Min = 0, .Max = 1, .Default = 0 },
   { .Key = "split", .Label = "Split",
     .Description = "Applies a separate width below the crossover frequency.",
     .Type = APT::BOOL, .Min = 0, .Max = 1, .Default = 0 },
   { .Key = "crossover", .Label = "Crossover",
     .Description = "The frequency below which the bass width applies instead of the width.",
     .Unit = APU::HZ, .Scale = APS::LOG, .Min = 40, .Max = 1000, .Default = 150, .Rules = glStereoSplitRules },
   { .Key = "bass_width", .Label = "Bass Width",
     .Description = "The level of the difference between the channels below the crossover.  Zero gives mono bass.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 200, .Default = 0, .Rules = glStereoSplitRules },
   { .Key = "gain", .Label = "Gain",
     .Description = "The output trim.",
     .Unit = APU::DB, .Min = -24, .Max = 12, .Default = 0 }
};

static const AudioOutputDesc glStereoOutputs[] = {
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", "Maximum input sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", "Maximum output sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::OUTPUT_PEAK }
};

//********************************************************************************************************************

static StereoSettings stereo_settings(const AudioParamState &State)
{
   return StereoSettings {
      .Width     = State.Params[ST_WIDTH],
      .Balance   = State.Params[ST_BALANCE],
      .Swap      = State.Params[ST_SWAP] != 0,
      .Split     = State.Params[ST_SPLIT] != 0,
      .Crossover = State.Params[ST_CROSSOVER],
      .BassWidth = State.Params[ST_BASS_WIDTH],
      .Gain      = State.Params[ST_GAIN]
   };
}

static void stereo_read(extAudioEffect *Effect, AudioParamState &State)
{
   const auto &settings = ((extAudioStereo *)Effect)->Settings;
   State.Params.assign({ settings.Width, settings.Balance, settings.Swap ? 1.0 : 0.0, settings.Split ? 1.0 : 0.0,
      settings.Crossover, settings.BassWidth, settings.Gain });
   State.Groups.clear();
}

//********************************************************************************************************************
// Before initialisation there is no processor.

static void stereo_apply(extAudioEffect *Effect, const AudioParamState &State)
{
   ((extAudioStereo *)Effect)->Settings = stereo_settings(State);
}

//********************************************************************************************************************
// Targets are derived outside the mixer lock.  An unsupported rate leaves the target invalid, in which case only the
// settings are published and the processor remains inactive.

class StereoUpdate final : public AudioParamUpdate {
public:
   StereoSettings Settings;
   StereoTarget Target;
   bool Valid;

   StereoUpdate(const AudioParamState &State, int Rate) : Settings(stereo_settings(State)) {
      Valid = stereo_target(Settings, Rate, Target);
   }

   void publish(extAudioEffect *Effect) override {
      auto Self = (extAudioStereo *)Effect;
      if (Self->Processor) Self->Processor->update(Settings, Valid ? &Target : nullptr);
      Self->Settings = Settings;
   }
};

static std::unique_ptr<AudioParamUpdate> stereo_prepare(extAudioEffect *Effect, const AudioParamState &State,
   int Rate)
{
   return std::make_unique<StereoUpdate>(State, Rate);
}

//********************************************************************************************************************

static const AudioEffectSchema glStereoSchema = {
   .ClassName   = "AudioStereo",
   .Version     = 1,
   .Description = "Adjusts the stereo width, balance and channel order of the sound.",
   .Params      = glStereoParams,
   .Outputs     = glStereoOutputs,
   .Read        = stereo_read,
   .Apply       = stereo_apply,
   .Prepare     = stereo_prepare
};

//********************************************************************************************************************

extAudioStereo::extAudioStereo(objMetaClass *ClassPtr, OBJECTID ObjectID) : extAudioEffect(ClassPtr, ObjectID)
{
   Schema = &glStereoSchema;
}

//********************************************************************************************************************

static ERR AUDIOSTEREO_Init(extAudioStereo *Self)
{
   if (Self->Processor) return ERR::InvalidState;
   auto chain = Self->Chain.lock();
   if (not chain) return ERR::NotInitialised;

   AudioParamState state;
   stereo_read(Self, state);
   {
      std::lock_guard mixer_lock(*chain->Mutex);
      if (validate_state(glStereoSchema, state, Self->OutputRate) != ERR::Okay) return ERR::InvalidValue;
   }

   auto processor = std::make_unique<StereoProcessor>(Self, Self->Settings);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;

   return error;
}

//********************************************************************************************************************

#include "class_audiostereo_def.c"

//********************************************************************************************************************

static ERR add_audiostereo_class()
{
   clAudioStereo = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIOEFFECT),
      fl::ClassID(CLASSID::AUDIOSTEREO),
      fl::Name("AudioStereo"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioStereoActions),
      fl::Size(sizeof(extAudioStereo)),
      fl::Path(MOD_PATH));
   return clAudioStereo ? ERR::Okay : ERR::AddClass;
}
