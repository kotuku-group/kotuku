/*********************************************************************************************************************

-CLASS-
AudioDelay: A feedback delay that adds filtered echoes, with optional stereo ping-pong routing.

The delay repeats its input after a fixed interval.  Each repeat is fed back into the delay at a reduced level, so
the echoes fade away over time, and a low-pass filter in the feedback path softens each repeat in turn.

Create new delay objects as a child of an @Audio object or set the inherited #AudioEffect.Audio field.  Use
#AudioEffect.Channel to process one channel set, or leave it at zero to process the global mix.  Application delays
accept live changes; global delays become immutable after initialisation.

Parameters are published through the inherited @AudioEffect schema and are changed with @AudioEffect.SetKey()
followed by @AudioEffect.Flush():

<list type="bullet">
<li>`time`: the interval between echoes, from 1 to 2000 milliseconds.  Fractions of a millisecond are honoured.</li>
<li>`feedback`: the level of each repeat relative to the previous one, from 0 to 95 percent.  Zero produces a single
echo.</li>
<li>`damping`: the high-frequency loss applied to every repeat, from 0 to 100 percent.  The first echo is never
filtered.</li>
<li>`mode`: `normal` (0) delays each channel independently; `ping_pong` (1) bounces the echoes between the left and
right channels.</li>
<li>`mix`: the linear blend of dry and wet signals.  Zero passes the input unchanged and 100 percent outputs only the
echoes.</li>
</list>

The following adds a ping-pong echo of 375 ms with gently darkening repeats to a playing channel set, as a single
change:

<pre>
delay = obj.new('AudioDelay', { audio=audio, channel=channel })
delay.acSetKey('time', 375)
delay.acSetKey('feedback', 45)
delay.acSetKey('damping', 40)
delay.acSetKey('mode', 1)
delay.acFlush()
</pre>

<header>Routing</header>

In normal mode every channel has its own delay line, so a stereo image is preserved in the echoes and a mono output is
processed as a single channel.  With feedback `g`, an impulse produces echoes at one, two and three times the delay,
at levels of 1, g and g² before damping and the wet mix are applied.

In ping-pong mode the average of the left and right inputs enters the left delay line and every repeat crosses to the
opposite side, so the first echo is heard on the left and later echoes alternate right, left, and so on.  The dry
signal is unchanged.  Because the input is averaged, a signal that is identical in both channels produces echoes at
its own level, but content with opposite polarity in the two channels cancels and produces no echo.  A mono output
always uses normal routing; the `mode` value is retained and applies if the output becomes stereo.

<header>Damping</header>

Damping is a one-pole low-pass filter in the feedback path.  The percentage `p` maps to a corner frequency of
`Fmax * (200 / Fmax)^(p / 100)` Hz, where `Fmax` is 20 kHz or 45% of the output rate if that is lower, so 50 percent
gives about 2 kHz at 48 kHz and 100 percent gives 200 Hz at every rate.  The filter coefficient is
`1 - exp(-2 pi fc / rate)`.  These corners describe the mapping rather than a measured -3 dB point, which is lower
than the nominal corner when it is close to the Nyquist frequency.  Zero damping is exactly unfiltered, and the filter
is blended in across the lowest 1 percent so that the sound does not jump between zero and a small value.  Damping
never boosts any frequency and does not reduce the level of low frequencies, so the repeats always decay at the rate
set by `feedback` or faster.

<header>Levels and Headroom</header>

Levels are measured in dBFS, where a sample magnitude of 1.0 is 0 dBFS, independently of the output format.  The
delay does not limit or clip its feedback loop.  A sustained input that reinforces its own echoes can build up to
`1 / (1 - g)` times its level in the delay line, which is 20 times (+26 dB) at the maximum feedback of 95 percent.
At high feedback and mix settings, leave headroom or place an @AudioLimiter after the delay.

Non-finite samples are outside the normalised mixing contract and are treated as silence.  Finite input with a
magnitude above 1e30 is clamped to that level so that the feedback loop cannot overflow.

<header>Live Changes</header>

Changes to feedback, damping, mix and mode ramp linearly over 10 ms and preserve the echoes that are already
present.  A mode change moves existing echoes to the new routing rather than discarding them.  A time change
crossfades from the old to the new delay over 10 ms rather than sweeping the delay, so there is no pitch change.  An
edit that arrives during a time crossfade replaces a single queued time, which is applied when the current crossfade
finishes.  Setting the mix to zero leaves the delay running, so raising it again reveals the echoes that are present.

Device reactivation, leaving bypass and reaching idle discard the echoes and apply the latest parameters without a
transition.

<header>Tail and Drain</header>

The delay has zero algorithmic latency, even at 100 percent mix: the echo time is the effect itself and is not
compensated.  After its input stops, the delay remains pending until every value in its delay memory and filter has
stayed at least 120 dB below the largest input sample since it was last cleared, for long enough to overwrite that
memory (2 seconds).  This includes silent gaps between widely spaced echoes.  The residual is relative to the input,
so quiet sources are not cut short.  Once the delay is no longer pending its remaining history is discarded.

When other audio shares the delay, a stopped voice's tail is estimated as the time for its echoes to fall by 60 dB.
Long settings produce long tails: at the maximum time of 2 seconds and feedback of 95 percent the estimate exceeds
four minutes, and draining to the residual bound takes longer still.  Draining is limited by @Audio.MaxDrain, which
then fades and discards the remainder.  Hard bypass and destruction cut the tail abruptly.

Output rates from 8000 to 192000 Hz are supported.  Configuring the effect for any other rate fails with
`ERR::NoSupport`.  The delay memory always covers the maximum time of 2 seconds and requires approximately 750 KB at
48 kHz in stereo, scaling in proportion to the output rate.

-END-

*********************************************************************************************************************/

#include "audio_delay_dsp.h"

class extAudioDelay : public extAudioEffect {
public:
   DelaySettings Settings;
   DelayProcessor *Processor = nullptr;

   extAudioDelay(objMetaClass *ClassPtr, OBJECTID ObjectID);
};

//********************************************************************************************************************
// Parameter descriptors.  Order matches the DL_ indexes.

enum { DL_TIME = 0, DL_FEEDBACK, DL_DAMPING, DL_MODE, DL_MIX };

static const AudioParamOption glDelayModes[] = {
   { DELAY_NORMAL, "normal", "Normal", "Every channel is delayed independently, preserving the stereo image." },
   { DELAY_PING_PONG, "ping_pong", "Ping-Pong",
     "The echoes alternate between the left and right channels, starting on the left.  Mono output is unaffected." }
};

static const AudioParamDesc glDelayParams[] = {
   { .Key = "time", .Label = "Time",
     .Description = "The interval between echoes.",
     .Unit = APU::MS, .Scale = APS::LOG, .Min = 1, .Max = DELAY_MAX_TIME, .Default = 250 },
   { .Key = "feedback", .Label = "Feedback",
     .Description = "The level of each repeat relative to the previous one.  Zero produces a single echo.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 95, .Default = 30 },
   { .Key = "damping", .Label = "Damping",
     .Description = "High-frequency loss applied to every repeat, so that later echoes sound progressively darker.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 100, .Default = 25 },
   { .Key = "mode", .Label = "Mode",
     .Description = "Whether stereo echoes are independent for each channel or alternate between the channels.",
     .Type = APT::ENUM, .Default = DELAY_NORMAL, .Options = glDelayModes },
   { .Key = "mix", .Label = "Mix",
     .Description = "The proportion of echoes in the output.  Zero leaves the input unchanged.",
     .Unit = APU::PERCENT, .Min = 0, .Max = 100, .Default = 25 }
};

static const AudioOutputDesc glDelayOutputs[] = {
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", "Maximum input sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", "Maximum output sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::OUTPUT_PEAK }
};

//********************************************************************************************************************

static DelaySettings delay_settings(const AudioParamState &State)
{
   return DelaySettings {
      .Time     = State.Params[DL_TIME],
      .Feedback = State.Params[DL_FEEDBACK],
      .Damping  = State.Params[DL_DAMPING],
      .Mode     = int(State.Params[DL_MODE]),
      .Mix      = State.Params[DL_MIX]
   };
}

static void delay_read(extAudioEffect *Effect, AudioParamState &State)
{
   const auto &settings = ((extAudioDelay *)Effect)->Settings;
   State.Params.assign({ settings.Time, settings.Feedback, settings.Damping, double(settings.Mode), settings.Mix });
   State.Groups.clear();
}

//********************************************************************************************************************
// Before initialisation there is no processor.

static void delay_apply(extAudioEffect *Effect, const AudioParamState &State)
{
   ((extAudioDelay *)Effect)->Settings = delay_settings(State);
}

//********************************************************************************************************************
// Coefficients and decay bounds are derived outside the mixer lock.  An unsupported rate leaves the target invalid,
// in which case only the settings are published and the processor remains inactive.

class DelayUpdate final : public AudioParamUpdate {
public:
   DelaySettings Settings;
   DelayTarget Target;
   bool Valid;

   DelayUpdate(const AudioParamState &State, int Rate) : Settings(delay_settings(State)) {
      Valid = delay_target(Settings, Rate, Target);
   }

   void publish(extAudioEffect *Effect) override {
      auto Self = (extAudioDelay *)Effect;
      if (Self->Processor) Self->Processor->update(Settings, Valid ? &Target : nullptr);
      Self->Settings = Settings;
   }
};

static std::unique_ptr<AudioParamUpdate> delay_prepare(extAudioEffect *Effect, const AudioParamState &State, int Rate)
{
   return std::make_unique<DelayUpdate>(State, Rate);
}

//********************************************************************************************************************

static const AudioEffectSchema glDelaySchema = {
   .ClassName   = "AudioDelay",
   .Version     = 1,
   .Description = "A feedback delay that adds filtered echoes, with optional stereo ping-pong routing.",
   .Params      = glDelayParams,
   .Outputs     = glDelayOutputs,
   .Read        = delay_read,
   .Apply       = delay_apply,
   .Prepare     = delay_prepare
};

//********************************************************************************************************************

extAudioDelay::extAudioDelay(objMetaClass *ClassPtr, OBJECTID ObjectID) : extAudioEffect(ClassPtr, ObjectID)
{
   Schema = &glDelaySchema;
}

//********************************************************************************************************************
// Storage is allocated by set_processor() outside the mixer lock.

static ERR AUDIODELAY_Init(extAudioDelay *Self)
{
   if (Self->Processor) return ERR::InvalidState;
   auto chain = Self->Chain.lock();
   if (not chain) return ERR::NotInitialised;

   AudioParamState state;
   delay_read(Self, state);
   {
      std::lock_guard mixer_lock(*chain->Mutex);
      if (validate_state(glDelaySchema, state, Self->OutputRate) != ERR::Okay) return ERR::InvalidValue;
   }

   auto processor = std::make_unique<DelayProcessor>(Self, Self->Settings);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;

   return error;
}

//********************************************************************************************************************

#include "class_audiodelay_def.c"

//********************************************************************************************************************

static ERR add_audiodelay_class()
{
   clAudioDelay = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIOEFFECT),
      fl::ClassID(CLASSID::AUDIODELAY),
      fl::Name("AudioDelay"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioDelayActions),
      fl::Size(sizeof(extAudioDelay)),
      fl::Path(MOD_PATH));
   return clAudioDelay ? ERR::Okay : ERR::AddClass;
}
