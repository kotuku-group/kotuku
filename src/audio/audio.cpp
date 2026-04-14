/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-MODULE-
Audio: Comprehensive audio processing and playback system with professional-grade mixing capabilities.

The Audio module provides a robust, cross-platform audio infrastructure that manages the complete audio pipeline from sample
loading through to hardware output.  The module's architecture supports both high-level convenience interfaces and low-level
professional audio control, making it suitable for applications ranging from simple media playback to sophisticated audio
production environments.

The module implements a client-server design pattern with two complementary class interfaces:

<list type="bullet">
<li>@Audio: Low-level hardware interface providing precise control over audio mixing, buffering, and output configuration.  Designed for applications requiring professional audio capabilities including real-time processing, multi-channel mixing, and advanced streaming architectures</li>
<li>@Sound: High-level sample playback interface optimised for simplicity and performance.  Automatically manages resource allocation, format conversion, and hardware abstraction whilst providing intelligent streaming decisions</li>
</list>

The internal mixer is a floating-point engine that processes all audio internally at 32-bit precision regardless of
output bit depth.  The mixer supports features that include:

<list type="bullet">
<li>Oversampling with interpolation for enhanced quality and reduced aliasing</li>
<li>Real-time volume ramping to eliminate audio artefacts during playback transitions</li>
<li>Multi-stage filtering with configurable low-pass and high-pass characteristics</li>
<li>Sample-accurate playback positioning with sub-sample interpolation</li>
<li>Bidirectional and unidirectional looping with precision loop point handling</li>
</list>

<header>Platform-Specific Optimisations</header>

Audio implementation is optimised for each supported platform to maximise performance and minimise latency:

<list type="bullet">
<li><b>Linux (ALSA Integration):</b> Utilises ALSA's period-based buffering with configurable period counts and sizes.  All samples are processed through the unified mixer with support for system-wide volume control and hardware mixer integration</li>
<li><b>Windows (DirectSound Integration):</b> Implements dual-path audio where simple playback can bypass the internal mixer for reduced latency, whilst complex operations utilise the full mixing pipeline.  Automatic fallback ensures compatibility across Windows audio driver variations</li>
<li><b>Cross-Platform Consistency:</b> API behaviour remains consistent across platforms, with platform-specific optimisations operating transparently to applications</li>
</list>

<header>Streaming and Memory Management</header>

The module implements streaming technology that automatically adapts to sample characteristics and system resources:

<list type="bullet">
<li>Smart streaming decisions based on sample size, available memory, and playback requirements</li>
<li>Configurable streaming thresholds with support for forced streaming or memory-resident operation</li>
<li>Rolling buffer architecture for large samples with automatic buffer management</li>
<li>Loop-aware streaming that maintains loop points during streaming operations</li>
</list>

<header>Usage Guidelines</header>

For optimal results, choose the appropriate interface based on application requirements:

<list type="bullet">
<li><b>@Sound Class (Recommended for Most Applications):</b> Provides immediate audio playback with automatic resource management, format detection, and intelligent streaming.  Ideal for media players, games, and general-purpose audio applications</li>
<li><b>@Audio Class (Advanced Applications):</b> Offers complete control over the audio pipeline including custom mixer configurations, real-time effects processing, and professional-grade timing control.  Required for audio workstations, synthesisers, and applications with demanding audio requirements</li>
</list>

<header>Technical Specifications</header>

<list type="bullet">
<li>Internal processing: 32-bit floating-point precision</li>
<li>Output formats: 8, 16, 24, and 32-bit with automatic conversion</li>
<li>Sample rates: Up to 44.1 kHz (hardware dependent)</li>
<li>Channel configurations: Mono and stereo with automatic format adaptation</li>
<li>Latency: Platform-optimised with configurable buffering for real-time applications</li>
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
#include <kotuku/strings.hpp>
#include <sstream>
#include <algorithm>

static ERR MODInit(OBJECTPTR, struct CoreBase *);
static ERR MODExpunge(void);
static ERR MODOpen(OBJECTPTR);

#include "module_def.c"

JUMPTABLE_CORE
static OBJECTPTR glAudioModule = nullptr;
static OBJECTPTR clAudio = 0;
static ankerl::unordered_dense::map<OBJECTID, int> glSoundChannels;
class extAudio;

ERR add_audio_class(void);
ERR add_sound_class(void);
void free_audio_class(void);
void free_sound_class(void);

extern "C" void end_of_stream(OBJECTPTR, int);

static void audio_stopped_event(extAudio &, int);
static ERR set_channel_volume(extAudio *, struct AudioChannel *);
static void load_config(extAudio *);
static ERR init_audio(extAudio *);
static ERR audio_timer(extAudio *, int64_t, int64_t);

#include "audio.h"

//********************************************************************************************************************

#ifdef _WIN32
#define USE_WIN32_PLAYBACK TRUE // All Sound objects get an independent DirectSound channel if enabled
#include "windows.h"

extern "C" char * dsInitDevice(int);
extern "C" void dsCloseDevice(void);
#endif

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
   pf::Log log;

   CoreBase = argCoreBase;
   glAudioModule = argModule;

#ifdef _WIN32
   {
      CSTRING errstr;
      if ((errstr = dsInitDevice(44100))) {
         log.warning("DirectSound Failed: %s", errstr);
         return ERR::NoSupport;
      }
   }
#elif ALSA_ENABLED
   // Nothing required for ALSA
#else
   log.warning("No audio support available.");
   return ERR::Failed;
#endif

   if (add_audio_class() != ERR::Okay) return ERR::AddClass;
   if (add_sound_class() != ERR::Okay) return ERR::AddClass;
   return ERR::Okay;
}

static ERR MODOpen(OBJECTPTR Module)
{
   Module->set(FID_FunctionList, glFunctions);
   return ERR::Okay;
}

static ERR MODExpunge(void)
{
   for (auto& [id, handle] : glSoundChannels) {
      // NB: Most Audio objects will be disposed of prior to this module being expunged.
      if (handle) {
         pf::ScopedObjectLock<extAudio> audio(id, 3000);
         if (audio.granted()) audio->closeChannels(handle);
      }
   }
   glSoundChannels.clear();

   free_audio_class();
   free_sound_class();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "alsa.cpp"
#include "functions.cpp"
#include "mixers.cpp"
#include "commands.cpp"
#include "class_audio.cpp"
#include "class_sound.cpp"
#include "mixer_dispatch.cpp"

//********************************************************************************************************************

static STRUCTS glStructures = {
   { "AudioLoop", sizeof(AudioLoop) }
};

KOTUKU_MOD(MODInit, nullptr, MODOpen, MODExpunge, nullptr, MOD_IDL, &glStructures)
extern "C" struct ModHeader * register_audio_module() { return &ModHeader; }
