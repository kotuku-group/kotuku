/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-MODULE-
Audio: Sample mixing and playback system with a floating-point mixer and platform audio output.

The Audio module manages the audio pipeline from sample loading through to hardware output.  It provides three class
interfaces:

<list type="bullet">
<li>@Sound: High-level sample playback.  Loads WAVE files by default and supports an MP3 extension; manages its own resources and decides whether a sample is
played from memory or streamed.  This is the recommended interface for most applications.</li>
<li>@Audio: Low-level mixer and device interface.  Provides channel sets, sample buffers and streams, command
sequencing, and control over output rate, bit depth, buffering and mixing quality.</li>
<li>@AudioEffect: Base class for DSP processors that attach to a channel set or to the global mix of an @Audio
object.</li>
</list>

The internal mixer processes all audio as normalised 32-bit floating-point data regardless of the output bit depth.
A magnitude of 1.0 is 0 dBFS, while intermediate buses retain finite values above unity as headroom.  The final output
conversion is the only stage that clips and quantises samples for the selected hardware format.  The mixer supports:

<list type="bullet">
<li>Linear interpolation between sample frames when `ADF::OVER_SAMPLING` is enabled, reducing aliasing when samples are
played at rates that differ from the output rate.</li>
<li>Volume ramping to avoid clicks when channel volumes change, enabled by `ADF::VOL_RAMPING` in combination with
`ADF::OVER_SAMPLING`.</li>
<li>Two levels of low-pass output smoothing, selected with `ADF::FILTER_LOW` or `ADF::FILTER_HIGH`.  The @Audio.Quality
field configures the filtering and oversampling flags as a group.</li>
<li>Unidirectional and bidirectional sample looping, including a secondary loop region.</li>
<li>Per-channel and global effect chains built from @AudioEffect processors.</li>
</list>

Output behaviour differs by platform:

<list type="bullet">
<li><b>Linux (ALSA):</b> All playback, including @Sound objects, is mixed by the internal mixer and written by a
dedicated worker thread.  Buffering is configured with the @Audio.Periods and @Audio.PeriodSize fields, and the
ALSA mixer is used for system volume control.</li>
<li><b>Windows (WASAPI):</b> All Sounds use the internal mixer and its application/global effects.  Each active
Audio object owns an event-driven shared-mode stream at the endpoint mix rate.  IAudioClient3 uses the supported
default period; event-driven IAudioClient is the compatibility fallback.  Windows 10 or later is supported.</li>
</list>

The @Sound.Stream field determines whether a sample is loaded into memory or streamed from its source.  With
`STREAM::SMART`, samples larger than 256KB are streamed; with `STREAM::ALWAYS`, samples larger than 16KB are
streamed; with `STREAM::NEVER`, the sample is always memory-resident.  Loop points are preserved when a sample is
streamed.

Technical specifications:

<list type="bullet">
<li>Internal processing: Normalised 32-bit floating-point with headroom above 0 dBFS.</li>
<li>Output formats: 8-bit and 16-bit integer, and 32-bit floating-point.</li>
<li>Output rates: Up to 192 kHz, subject to hardware negotiation.  Windows follows the endpoint mix rate.</li>
<li>Channel configurations: Mono and stereo output.  Mono and stereo samples are converted to the output layout during
mixing.</li>
</list>

-END-

*********************************************************************************************************************/

#define PRV_AUDIO_MODULE

#ifdef __linux__
 #include <sys/ioctl.h>
 #include <fcntl.h>
 #ifdef __ANDROID__
  #include <linux/soundcard.h>
  #include "kd.h"
 #else
  #include <sys/soundcard.h>
  #include <sys/kd.h>
 #endif
 #include <unistd.h>
 #ifdef ALSA_ENABLED
  #include <alsa/asoundlib.h> // Requires libasound2-dev
 #endif
#endif

#include <kotuku/main.h>
#include <kotuku/modules/audio.h>
#include <kotuku/modules/filesystem.h>
#include <kotuku/modules/processes.h>
#include <kotuku/modules/config.h>
#include <kotuku/modules/script.h>
#include <kotuku/modules/module.h>
#include <kotuku/strings.hpp>
#include <sstream>
#include <algorithm>

static ERR MODInit(OBJECTPTR, struct CoreBase *);
static ERR MODExpunge(void);
static ERR MODOpen(OBJECTPTR);

#include "module_def.c"

JUMPTABLE_CORE
static OBJECTPTR clAudio = 0;
static OBJECTPTR clAudioEffect = nullptr;
static OBJECTPTR clAudioEqualiser = nullptr;
static OBJECTPTR clAudioReverb = nullptr;
static OBJECTPTR clAudioCompressor = nullptr;
static OBJECTPTR clAudioLimiter = nullptr;
static OBJECTPTR clAudioGate = nullptr;
static OBJECTPTR clAudioDelay = nullptr;
static OBJECTPTR clAudioChorus = nullptr;
static OBJECTPTR clAudioFlanger = nullptr;
static OBJECTPTR clAudioPhaser = nullptr;
static OBJECTPTR clAudioLoudness = nullptr;
static OBJECTPTR clAudioSaturator = nullptr;
static OBJECTPTR clAudioSplitter = nullptr;
static OBJECTPTR clAudioStereo = nullptr;
static OBJECTPTR clAudioAnalyser = nullptr;
static ERR add_audioeffect_class();
static ERR add_audioequaliser_class();
static ERR add_audioreverb_class();
static ERR add_audiocompressor_class();
static ERR add_audiolimiter_class();
static ERR add_audiogate_class();
static ERR add_audiodelay_class();
static ERR add_audiochorus_class();
static ERR add_audioflanger_class();
static ERR add_audiophaser_class();
static ERR add_audioloudness_class();
static ERR add_audiosaturator_class();
static ERR add_audiosplitter_class();
static ERR add_audiostereo_class();
static ERR add_audioanalyser_class();
static ankerl::unordered_dense::map<OBJECTID, int> glSoundChannels;
static std::string glAudioDevice;
class extAudio;

ERR add_audio_class(void);
ERR add_sound_class(void);
void free_audio_class(void);
void free_sound_class(void);

static void audio_stopped_event(extAudio &, int);
static ERR set_channel_volume(extAudio *, struct AudioChannel *);
static ERR init_audio(extAudio *);

#if defined(_WIN32) or defined(ALSA_ENABLED)
#define AUDIO_WORKER
#endif
#include "audio.h"

//********************************************************************************************************************

#ifdef ALSA_ENABLED
static void free_alsa(extAudio *);

static const int16_t glAlsaConvert[6] = {
   SND_MIXER_SCHN_FRONT_LEFT,   // Conversion table must follow the CHN_ order
   SND_MIXER_SCHN_FRONT_RIGHT,
   SND_MIXER_SCHN_FRONT_CENTER,
   SND_MIXER_SCHN_REAR_LEFT,
   SND_MIXER_SCHN_REAR_RIGHT,
   SND_MIXER_SCHN_WOOFER
};
#endif

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   kt::Log log;

   CoreBase = argCoreBase;

#if defined(_WIN32) or defined(ALSA_ENABLED)
   std::span<std::string> args;
   auto task = CurrentTask();
   if (!task->getParameters(args)) {
      for (int i=0; i < std::ssize(args); i++) {
         if (kt::iequals(args[i], "--audio-device")) {
            if (i + 1 < std::ssize(args)) {
               glAudioDevice = args[i + 1];
               log.msg("Audio output device set to \"%s\".", glAudioDevice.c_str());
               i++;
            }
         }
      }
   }
#else
   log.warning("No audio support available.");
   return ERR::NoSupport;
#endif

   if (add_audio_class() != ERR::Okay) return ERR::AddClass;
   if (add_audioeffect_class() != ERR::Okay) return ERR::AddClass;
   if (add_audioequaliser_class() != ERR::Okay) return ERR::AddClass;
   if (add_audioreverb_class() != ERR::Okay) return ERR::AddClass;
   if (add_audiocompressor_class() != ERR::Okay) return ERR::AddClass;
   if (add_audiolimiter_class() != ERR::Okay) return ERR::AddClass;
   if (add_audiogate_class() != ERR::Okay) return ERR::AddClass;
   if (add_audiodelay_class() != ERR::Okay) return ERR::AddClass;
   if (add_audiochorus_class() != ERR::Okay) return ERR::AddClass;
   if (add_audioflanger_class() != ERR::Okay) return ERR::AddClass;
   if (add_audiophaser_class() != ERR::Okay) return ERR::AddClass;
   if (add_audioloudness_class() != ERR::Okay) return ERR::AddClass;
   if (add_audiosaturator_class() != ERR::Okay) return ERR::AddClass;
   if (add_audiosplitter_class() != ERR::Okay) return ERR::AddClass;
   if (add_audiostereo_class() != ERR::Okay) return ERR::AddClass;
   if (add_audioanalyser_class() != ERR::Okay) return ERR::AddClass;
   if (add_sound_class() != ERR::Okay) return ERR::AddClass;
   return ERR::Okay;
}

static ERR MODOpen(OBJECTPTR Module)
{
   ((objModule *)Module)->setFunctionList(glFunctions);
   return ERR::Okay;
}

static ERR MODExpunge(void)
{
   for (auto & [id, handle] : glSoundChannels) {
      // NB: Most Audio objects will be disposed of prior to this module being expunged.
      if (handle) {
         kt::ScopedObjectLock<extAudio> audio(id, 3000);
         if (audio.granted()) audio->closeChannels(handle);
      }
   }
   glSoundChannels.clear();

   if (clAudioAnalyser) { FreeResource(clAudioAnalyser); clAudioAnalyser = nullptr; }
   if (clAudioStereo) { FreeResource(clAudioStereo); clAudioStereo = nullptr; }
   if (clAudioSplitter) { FreeResource(clAudioSplitter); clAudioSplitter = nullptr; }
   if (clAudioSaturator) { FreeResource(clAudioSaturator); clAudioSaturator = nullptr; }
   if (clAudioLoudness) { FreeResource(clAudioLoudness); clAudioLoudness = nullptr; }
   if (clAudioPhaser) { FreeResource(clAudioPhaser); clAudioPhaser = nullptr; }
   if (clAudioFlanger) { FreeResource(clAudioFlanger); clAudioFlanger = nullptr; }
   if (clAudioChorus) { FreeResource(clAudioChorus); clAudioChorus = nullptr; }
   if (clAudioDelay) { FreeResource(clAudioDelay); clAudioDelay = nullptr; }
   if (clAudioGate) { FreeResource(clAudioGate); clAudioGate = nullptr; }
   if (clAudioLimiter) { FreeResource(clAudioLimiter); clAudioLimiter = nullptr; }
   if (clAudioCompressor) { FreeResource(clAudioCompressor); clAudioCompressor = nullptr; }
   if (clAudioReverb) { FreeResource(clAudioReverb); clAudioReverb = nullptr; }
   if (clAudioEqualiser) { FreeResource(clAudioEqualiser); clAudioEqualiser = nullptr; }
   if (clAudioEffect) { FreeResource(clAudioEffect); clAudioEffect = nullptr; }
   free_audio_class();
   free_sound_class();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "alsa.cpp"
#include "functions.cpp"
#include "mixers.cpp"
#include "commands.cpp"
#include "render_client.cpp"
#include "alsa_worker.cpp"
#include "windows_worker.cpp"
#include "audio_effect.cpp"
#include "class_audioeffect.cpp"
#include "class_audioequaliser.cpp"
#include "class_audioreverb.cpp"
#include "class_audiocompressor.cpp"
#include "class_audiolimiter.cpp"
#include "class_audiogate.cpp"
#include "class_audiodelay.cpp"
#include "class_audiochorus.cpp"
#include "class_audioflanger.cpp"
#include "class_audiophaser.cpp"
#include "class_audioloudness.cpp"
#include "class_audiosaturator.cpp"
#include "class_audiosplitter.cpp"
#include "class_audiostereo.cpp"
#include "class_audioanalyser.cpp"
#include "class_audio.cpp"
#include "class_sound.cpp"
#include "mixer_dispatch.cpp"

//********************************************************************************************************************

#ifdef UNIT_TESTS
#include "tests/unit_tests.h"
#include "tests/test_audio_batches.cpp"
#include "tests/test_audio_buffer.cpp"
#include "tests/test_audio_effect.cpp"
#include "tests/test_dsp_infrastructure.cpp"
#include "tests/test_audio_equaliser_dsp.cpp"
#include "tests/test_audio_reverb_dsp.cpp"
#include "tests/test_audio_compressor_dsp.cpp"
#include "tests/test_audio_limiter_dsp.cpp"
#include "tests/test_audio_gate_dsp.cpp"
#include "tests/test_audio_delay_dsp.cpp"
#include "tests/test_audio_chorus_dsp.cpp"
#include "tests/test_audio_flanger_dsp.cpp"
#include "tests/test_audio_phaser_dsp.cpp"
#include "tests/test_audio_loudness_dsp.cpp"
#include "tests/test_audio_saturator_dsp.cpp"
#include "tests/test_audio_splitter_dsp.cpp"
#include "tests/test_audio_stereo_dsp.cpp"
#include "tests/test_audio_analyser_dsp.cpp"
#include "tests/test_mixers.cpp"
#include "tests/test_audio_format.cpp"
#ifdef ALSA_ENABLED
#include "tests/test_audio_worker.cpp"
#endif
#ifdef _WIN32
#include "tests/test_wasapi_transport.cpp"
#endif
#undef AUDIO_CHECK
#undef AUDIO_REQUIRE
#endif

static void MODTest(std::string_view Options, int *Passed, int *Total)
{
#ifdef UNIT_TESTS
   AudioTestContext test;
   auto run = [&test](const char *Name, auto Routine) {
      kt::Log log("AudioTests");
      log.branch("Running %s unit tests...", Name);
      const int passed = test.Passed.load(), total = test.Total.load();
      Routine(test);
      log.msg("Passed %d of %d checks.", test.Passed.load() - passed, test.Total.load() - total);
   };
   run("batch", audio_tests_audio_batches::run);
   run("buffer", audio_tests_audio_buffer::run);
   run("effect chain", audio_tests_audio_effect::run);
   run("DSP infrastructure", audio_tests_dsp_infrastructure::run);
   run("equaliser DSP", audio_tests_audio_equaliser_dsp::run);
   run("reverb DSP", audio_tests_audio_reverb_dsp::run);
   run("compressor DSP", audio_tests_audio_compressor_dsp::run);
   run("limiter DSP", audio_tests_audio_limiter_dsp::run);
   run("gate DSP", audio_tests_audio_gate_dsp::run);
   run("delay DSP", audio_tests_audio_delay_dsp::run);
   run("chorus DSP", audio_tests_audio_chorus_dsp::run);
   run("flanger DSP", audio_tests_audio_flanger_dsp::run);
   run("phaser DSP", audio_tests_audio_phaser_dsp::run);
   run("loudness DSP", audio_tests_audio_loudness_dsp::run);
   run("saturator DSP", audio_tests_audio_saturator_dsp::run);
   if (Options.find("saturator-spectra") != std::string_view::npos) {
      run("saturator spectral report", audio_tests_audio_saturator_dsp::spectra);
   }
   run("splitter DSP", audio_tests_audio_splitter_dsp::run);
   run("stereo DSP", audio_tests_audio_stereo_dsp::run);
   run("analyser DSP", audio_tests_audio_analyser_dsp::run);
   run("mixer", audio_tests_mixers::run);
   run("format", audio_tests_audio_format::run);
#ifdef ALSA_ENABLED
   run("ALSA worker", audio_tests_audio_worker::run);
#endif
#ifdef _WIN32
   run("WASAPI transport", audio_tests_wasapi_transport::run);
#endif
   *Passed += test.Passed.load();
   *Total += test.Total.load();
#else
   kt::Log("AudioTests").warning("Unit tests are disabled in this build.");
#endif
}

//********************************************************************************************************************

static ModHeader::STRUCTS glStructures = {
   { "AudioMixCommand", { sizeof(AudioMixCommand), alignof(AudioMixCommand) } },
   { "AudioLoop",       { sizeof(AudioLoop), alignof(AudioLoop) } },
   { "AudioFormat",     { sizeof(AudioFormat), alignof(AudioFormat) } },
   { "MeterInfo",       { sizeof(MeterInfo), alignof(MeterInfo) } },
   { "MeterLayout",     { sizeof(MeterLayout), alignof(MeterLayout) } },
   { "MeterReading",    { sizeof(MeterReading), alignof(MeterReading) } },
   { "MeterValue",      { sizeof(MeterValue), alignof(MeterValue) } }
};

KOTUKU_MOD(MODInit, nullptr, MODOpen, MODExpunge, MODTest, MOD_IDL, &glStructures)
extern "C" struct ModHeader * register_audio_module() { return &ModHeader; }
