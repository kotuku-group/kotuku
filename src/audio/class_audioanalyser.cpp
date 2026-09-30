/*********************************************************************************************************************

-CLASS-
AudioAnalyser: Measures the spectrum and waveform of audio in a mixing chain, for visualisation.

The analyser captures the audio that passes through its position in an effect chain without changing it.  Use it to
drive spectrum analysers, oscilloscopes and level displays that follow what the listener hears.

Create new analysers as a child of an @Audio object or set the inherited #AudioEffect.Audio field.  Use
#AudioEffect.Channel to measure one channel set, or leave it at zero to measure the global mix.  Set a high
#AudioEffect.Order to measure the output of the other effects in the chain.

Read the current spectrum with #GetSpectrum() and the current waveform with #GetWaveform().  Both are cheap enough to
call on every frame of an animation.  The following draws a ten band spectrum:

<pre>
analyser = obj.new('AudioAnalyser', { audio=sound.audio })
edges = array&lt;double&gt; { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000, 20000 }
levels = array&lt;double, 10&gt;
if analyser.mtGetSpectrum(edges, levels) is ERR_Okay then
   for i in {0 to 10} do bar_height(i, math.clamp((levels[i] + 60) / 60, 0, 1)) end
end
</pre>

<header>Synchronisation</header>

Audio is mixed ahead of the device, which then queues it before it is heard.  Readings are aligned with the audio that
is being heard, not the audio that was most recently mixed.  The analyser estimates the audible position from the
device queue and the time that has elapsed since the last mix.  Up to one second of device queue is compensated for.

After a source stops, the remaining queued audio is still reported until it has been heard, after which readings are
silent.  A bypassed analyser does not capture audio, so its readings fall silent in the same way.

<header>Measurement</header>

The analyser measures a mono downmix, which is the mean of every channel in the processing layout.  Audio that is out
of phase between channels therefore reads lower than it sounds.

The spectrum is computed with a Hann window over the most recent 32 to 47 ms of audio, depending on the output rate.
At 44.1 and 48 kHz the window is 2048 frames long and the frequency resolution is about 23 Hz.  Band levels are
calibrated so that a full-scale sinusoid within a band reads 0 dBFS.

In addition to the spectrum and waveform, the standard input and output peaks are published for each channel.  The
analyser adds no latency and has no tail.

Output rates from 8000 to 192000 Hz are supported.  Configuring the analyser for any other rate fails with
`ERR::NoSupport`.

-END-

*********************************************************************************************************************/

#include "audio_analyser_dsp.h"

class extAudioAnalyser : public extAudioEffect {
public:
   AnalyserProcessor *Processor = nullptr;

   extAudioAnalyser(objMetaClass *ClassPtr, OBJECTID ObjectID);
};

//********************************************************************************************************************

static const AudioOutputDesc glAnalyserOutputs[] = {
   { "spectrum", AudioOutputKind::SPECTRUM, "Spectrum",
      "Levels of frequency bands in the audio that is currently audible, read with GetSpectrum()." },
   { "waveform", AudioOutputKind::WAVEFORM, "Waveform",
      "The mono waveform that is currently audible, read with GetWaveform()." },
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", "Maximum input sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", "Maximum output sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::OUTPUT_PEAK }
};

// The analyser has no parameters.

static void analyser_read(extAudioEffect *Effect, AudioParamState &State)
{
   State.Params.clear();
   State.Groups.clear();
}

static void analyser_apply(extAudioEffect *Effect, const AudioParamState &State) { }

static const AudioEffectSchema glAnalyserSchema = {
   .ClassName   = "AudioAnalyser",
   .Version     = 1,
   .Description = "Measures the spectrum and waveform of audio in a mixing chain, for visualisation.",
   .Outputs     = glAnalyserOutputs,
   .Read        = analyser_read,
   .Apply       = analyser_apply
};

//********************************************************************************************************************

extAudioAnalyser::extAudioAnalyser(objMetaClass *ClassPtr, OBJECTID ObjectID) : extAudioEffect(ClassPtr, ObjectID)
{
   Schema = &glAnalyserSchema;
}

//********************************************************************************************************************
// Copies the audible frames into Output, oldest first.  If Frames is zero, the analysis window for the output rate is
// copied.  Output must be large enough for either, so that nothing is allocated while the mixer is locked.  The Audio
// object is locked for its device queue, which follows the same lock order as the Sound class.

static ERR analyser_capture(extAudioAnalyser *Self, std::vector<float> &Output, int Frames, int &Rate)
{
   auto chain = Self->Chain.lock();
   if ((not chain) or (not Self->Processor)) return ERR::NotInitialised;

   kt::ScopedObjectLock<extAudio> audio(Self->AudioID, 1000);
   if (not audio.granted()) return ERR::AccessObject;

   std::lock_guard mixer_lock(*chain->Mutex);
   const auto &processor = *Self->Processor;
   if ((processor.Rate < 2) or processor.History.empty()) return ERR::NotInitialised;

   Rate = processor.Rate;
   const int count = Frames ? Frames : analyser_window(Rate);
   if (count > std::ssize(Output)) return ERR::BufferOverflow;

   const auto end = processor.audible(PreciseTime(), audio->SubmitLag());
   processor.copy(end, std::span<float>(Output.data(), size_t(count)));
   Output.resize(size_t(count)); // Never reallocates
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetSpectrum: Measures the level of frequency bands in the audio that is currently audible.

GetSpectrum() measures the audible audio and returns the level of each band that is defined by `Frequencies`.  The
band edges are given in ascending order, so that N+1 edges define N bands, each of which spans from one edge to the
next.  Bands can have any width and need not be evenly spaced; logarithmic spacing suits most displays.

Levels are in dBFS, where a full-scale sinusoid within a band reads 0 dBFS, and the lowest reading is -120 dBFS.  The
energy of a sinusoid can spread into neighbouring bands if it lies close to an edge.  A band that is narrower than
the frequency resolution of the analyser reports an interpolated level, and is therefore smoothed.

-INPUT-
array(double) Frequencies: Ascending band edges in hertz, from zero to half of the #AudioEffect.OutputRate.
^array(double) Levels: Receives the level of each band in dBFS.  Must have one entry less than `Frequencies`.

-ERRORS-
Okay
NullArgs
Args: Fewer than two edges, more than 1025 edges, or `Levels` is not one entry shorter than `Frequencies`.
InvalidValue: The edges are not ascending, or lie outside of the range from zero to half of the output rate.
NotInitialised: The analyser is not connected to an effect chain.
AccessObject: The @Audio object could not be locked.
-END-

*********************************************************************************************************************/

static ERR AUDIOANALYSER_GetSpectrum(extAudioAnalyser *Self, struct ana::GetSpectrum *Args)
{
   if (not Args) return ERR::NullArgs;

   const auto &edges = Args->Frequencies;
   if ((edges.size() < 2) or (edges.size() > ANALYSER_MAX_BANDS + 1)) return ERR::Args;
   if (Args->Levels.size() + 1 != edges.size()) return ERR::Args;

   for (size_t i = 0; i < edges.size(); i++) {
      if ((not std::isfinite(edges[i])) or (edges[i] < 0)) return ERR::InvalidValue;
      if ((i > 0) and (edges[i] <= edges[i - 1])) return ERR::InvalidValue;
   }

   std::vector<float> samples(ANALYSER_MAX_WINDOW);
   int rate;
   if (auto error = analyser_capture(Self, samples, 0, rate); error != ERR::Okay) return error;
   if (edges.back() > double(rate) * 0.5) return ERR::InvalidValue;

   std::vector<double> power;
   analyser_power(samples, power);
   analyser_bands(power, double(rate) / double(samples.size()), edges, Args->Levels);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetWaveform: Reads the waveform of the audio that is currently audible.

GetWaveform() fills `Samples` with the most recent audible frames, oldest first.  Each value is the mean of every
channel in the processing layout, where a magnitude of 1.0 is full scale.  Frames that have not been captured, such as
those after a source has stopped, are zero.

The length of `Samples` determines the duration that is read, e.g. 1024 samples cover 21 ms at 48 kHz.

-INPUT-
^array(float) Samples: Receives the waveform.  Up to 16384 samples can be read.

-ERRORS-
Okay
NullArgs
Args: `Samples` is empty or longer than 16384 entries.
NotInitialised: The analyser is not connected to an effect chain.
AccessObject: The @Audio object could not be locked.
-END-

*********************************************************************************************************************/

static ERR AUDIOANALYSER_GetWaveform(extAudioAnalyser *Self, struct ana::GetWaveform *Args)
{
   if (not Args) return ERR::NullArgs;
   if (Args->Samples.empty() or (Args->Samples.size() > ANALYSER_MAX_WAVEFORM)) return ERR::Args;

   std::vector<float> samples(Args->Samples.size());
   int rate;
   if (auto error = analyser_capture(Self, samples, int(samples.size()), rate); error != ERR::Okay) return error;
   std::copy(samples.begin(), samples.end(), Args->Samples.begin());
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR AUDIOANALYSER_Init(extAudioAnalyser *Self)
{
   if (Self->Processor) return ERR::InvalidState;

   auto processor = std::make_unique<AnalyserProcessor>(Self);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;
   return error;
}

//********************************************************************************************************************

#include "class_audioanalyser_def.c"

static const FieldArray clAudioAnalyserFields[] = {
   END_FIELD
};

//********************************************************************************************************************

static ERR add_audioanalyser_class()
{
   clAudioAnalyser = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIOEFFECT),
      fl::ClassID(CLASSID::AUDIOANALYSER),
      fl::Name("AudioAnalyser"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioAnalyserActions),
      fl::Methods(clAudioAnalyserMethods),
      fl::Fields(clAudioAnalyserFields),
      fl::Size(sizeof(extAudioAnalyser)),
      fl::Path(MOD_PATH));
   return clAudioAnalyser ? ERR::Okay : ERR::AddClass;
}
