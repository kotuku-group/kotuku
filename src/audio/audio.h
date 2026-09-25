
using namespace kt;

#include <variant>
#include <mutex>
#include <optional>
#include <atomic>
#ifdef _WIN32
#include "wasapi.h"
#endif
#include <unordered_map>
#ifdef ALSA_ENABLED
#include <pthread.h>
#include <poll.h>
#include <sys/eventfd.h>
#endif
#include "audio_buffer.h"
#ifdef ALSA_ENABLED
#include "audio_worker.h"
#endif
#include "mixer_dispatch.h"
#include "audio_completions.h"

#define MIX_INTERVAL 0.01

enum SAMPLE : int {};
enum BYTELEN : int {};

inline BYTELEN operator + (BYTELEN a, BYTELEN b) { return BYTELEN(((int)a) + ((int)b)); }
inline BYTELEN &operator += (BYTELEN &a, BYTELEN b) { return (BYTELEN &)(((int &)a) += ((int)b)); }

inline SAMPLE &operator -= (SAMPLE &a, SAMPLE b) { return (SAMPLE &)(((int &)a) -= ((int)b)); }

inline void release_audio_callback(FUNCTION &Function)
{
   if (Function.defined()) {
      if (Function.isScript() and (not Function.stale())) ((objScript *)Function.Context)->derefProcedure(Function);
      Function.unpin();
      Function.disable();
   }
}

//********************************************************************************************************************
// Audio channel commands

enum class CMD : int {
   END_SEQUENCE=1,
   SAMPLE,
   VOLUME,
   PAN,
   FREQUENCY,
   TEMPO,
   STOP,
   STOP_LOOPING,
   POSITION,
   PLAY,
   MUTE,
   SET_LENGTH,
   CONTINUE,
   PAUSE
};

//********************************************************************************************************************
// Sample shift - value used for converting total data size down to samples.

inline const int sample_shift(const SFM Type)
{
   switch (Type) {
      default: return 0;
      case SFM::U8_BIT_STEREO:
      case SFM::S16_BIT_MONO: return 1;
      case SFM::S16_BIT_STEREO: return 2;
   }
   return 0;
}

//********************************************************************************************************************

struct WAVEFormat {
   int16_t Format;            // Type of WAVE data in the chunk: RAW or ADPCM
   int16_t Channels;          // Number of channels, 1=mono, 2=stereo
   int Frequency;             // Playback frequency
   int AvgBytesPerSecond;     // Channels * SamplesPerSecond * (BitsPerSample / 8)
   int16_t BlockAlign;        // Channels * (BitsPerSample / 8)
   int16_t BitsPerSample;     // Bits per sample
   int16_t ExtraLength;
};

// Function to set mixing step for thread-safe operation
void set_mix_step(int step);

static const int16_t WAVE_RAW   = 0x0001;  // Uncompressed waveform data.
static const int16_t WAVE_ADPCM = 0x0002;  // ADPCM compressed waveform data.
static const int16_t WAVE_FLOAT = 0x0003;  // Uncompressed floating point waveform


//********************************************************************************************************************

struct AudioSample {
   FUNCTION Callback;     // For feeding audio streams.
   FUNCTION OnStop;       // Called when playback stops.
   std::vector<uint8_t> Data; // Sample data.
   SAMPLE   Loop1Start;   // Start of the first loop
   SAMPLE   Loop1End;     // End of the first loop
   SAMPLE   Loop2Start;   // Start of the second loop
   SAMPLE   Loop2End;     // End of the second loop
   SAMPLE   SampleLength; // Length of the Data sample/buffer.  Measured in samples
   BYTELEN  StreamLength; // Streams only.  Total byte-length of the sample data that is being streamed.
   BYTELEN  PlayPos;      // Current read position relative to StreamLength/SampleLength, measured in bytes
   BYTELEN  BufferedLength; // Streams only.  Valid source bytes in the current rolling buffer.
   LOOP     LoopMode;     // Loop mode (single, double)
   SFM      SampleType;   // Type of sample (bit format)
   LTYPE    Loop1Type;    // First loop type (unidirectional, bidirectional)
   LTYPE    Loop2Type;    // Second loop type (unidirectional, bidirectional)
   #ifdef AUDIO_WORKER
   uint64_t Generation = 0;
   uint64_t DeferredStops = 0;
   int64_t DeferredStopDue = 0;
   AudioRingCursor Ring;
   int SourceOffset = 0;
   int64_t RetryAt = 0;
   bool Refilling = false;
   bool SourceSeek = true;
   bool RefillPending = false;
   bool Prefilled = false;
   bool Starved = false;
   bool EndOfSource = false;
   #endif
   bool     Stream;       // True if this is a stream

   AudioSample() {
      Stream   = false;
      clear();
   }

   AudioSample(AudioSample and) noexcept = default;
   AudioSample &operator=(AudioSample and) noexcept = default;
   AudioSample(const AudioSample &) = delete;
   AudioSample &operator=(const AudioSample &) = delete;

   ~AudioSample() {
      clear();
   }

   void clear() {
      #ifdef AUDIO_WORKER
      ++Generation;
      DeferredStops = 0;
      Ring.Read = Ring.Used = 0;
      RefillPending = Prefilled = Starved = EndOfSource = false;
      RetryAt = 0;
      Refilling = false;
      #endif
      Stream = false;
      Callback.clear();
      OnStop.clear();
      std::vector<uint8_t>().swap(Data);
      SampleLength = SAMPLE(0);
      Loop1Start   = SAMPLE(0);
      Loop1End     = SAMPLE(0);
      Loop2Start   = SAMPLE(0);
      Loop2End     = SAMPLE(0);
      StreamLength = BYTELEN(0);
      BufferedLength = BYTELEN(0);
      SampleType   = SFM::NIL;
      LoopMode     = LOOP::NIL;
      Loop1Type    = LTYPE::NIL;
      Loop2Type    = LTYPE::NIL;
   }
};

//********************************************************************************************************************

struct AudioCommand {
   int CompletionSlot = -1;
   int CommandIndex = 0;
   ERR DeferredError = ERR::Okay;
   CMD  CommandID;    // Command ID
   int Handle;       // Channel handle
   std::variant<double,int,bool> Data; // Special data related to the command ID

   AudioCommand(CMD pCommandID, int pHandle) :
      CommandID(pCommandID), Handle(pHandle), Data(double(0)) { }

   AudioCommand(CMD pCommandID, int pHandle, double pData) :
      CommandID(pCommandID), Handle(pHandle), Data(pData) { }

   AudioCommand(CMD pCommandID, int pHandle, int pData) :
      CommandID(pCommandID), Handle(pHandle), Data(pData) { }

   AudioCommand(CMD pCommandID, int pHandle, bool pData) :
      CommandID(pCommandID), Handle(pHandle), Data(pData) { }

   AudioCommand() = default;
};

//********************************************************************************************************************

struct AudioChannel {
   double   LVolume;        // Current left speaker volume after applying Pan (0 - 1.0)
   double   RVolume;        // Current right speaker volume after applying Pan (0 - 1.0)
   double   LVolumeTarget;  // Volume target when fading or ramping
   double   RVolumeTarget;  // Volume target when fading or ramping
   double   Volume;         // Playing volume (0 - 1.0)
   double   Pan;            // Pan value (-1.0 - 1.0)
   int64_t  EndTime;        // Anticipated end-time of playing the current sample, if OnStop is defined in the sample.
   uint64_t PlaybackGeneration; // Identifies the current use of this channel for deferred completion delivery.
   int      SampleHandle;   // Sample index, direct lookup into extAudio->Samples
   int      Handle;         // Public channel handle used to validate deferred completion delivery.
   CHF      Flags;          // Special flags
   int      Position;       // Current playing/mixing byte position within Sample.
   int      Frequency;      // Playback frequency
   int      PositionLow;    // Playing position, lower bits
   int8_t   Priority;       // Priority of the sound that has been assigned to this channel
   CHS      State;          // Channel state
   int8_t   LoopIndex;      // The current active loop (either 0, 1 or 2)

   bool active() {
      return Frequency ? true : false;
   }

   inline bool isStopped() {
      return ((State IS CHS::STOPPED) or (State IS CHS::FINISHED));
   }
};

class extAudioEffect;

#include "audio_effect_params.h"

//********************************************************************************************************************
// Separate from Object: a vtable must never precede the framework's fixed object header.

class AudioEffectProcessor {
public:
   virtual ~AudioEffectProcessor() = default;
   virtual void process(float *Buffer, int Frames) = 0;
   virtual void reset() = 0;
};

struct AudioEffectChain {
   std::shared_ptr<std::recursive_mutex> Mutex;
   std::vector<extAudioEffect *> Effects;
   uint64_t NextSequence = 0;

   explicit AudioEffectChain(std::shared_ptr<std::recursive_mutex> Lock) : Mutex(std::move(Lock)) { }
};

class extAudioEffect : public objAudioEffect {
public:
   // The weak reference allows Audio or a channel set to disappear before an externally owned effect.
   std::weak_ptr<AudioEffectChain> Chain;
   uint64_t Sequence = 0;
   bool ResetPending = true;
   // Set by subclass constructors that publish parameters.  Static storage; never freed.
   const AudioEffectSchema *Schema = nullptr;
   // Changes staged by SetParameter(), InsertEntry() and RemoveEntry() after initialisation, pending Flush().
   std::unique_ptr<AudioParamState> Pending;

   extAudioEffect(objMetaClass *ClassPtr, OBJECTID ObjectID) : objAudioEffect(ClassPtr, ObjectID) {
      AudioID = Channel = Order = OutputRate = Stereo = 0;
      Flags = AEF::NIL;
   }
   ~extAudioEffect();
   void detach();
   // Subclasses publish a fully configured processor as their final Init step.  No subsequent unsynchronised writes.
   ERR set_processor(std::unique_ptr<AudioEffectProcessor> Processor);
   void process(float *Buffer, int Frames);

private:
   std::unique_ptr<AudioEffectProcessor> processor;
};

static void process_effects(AudioEffectChain &, float *, int);
static void configure_effects(AudioEffectChain &, int, bool);

//********************************************************************************************************************

struct ChannelSet {
   std::vector<AudioChannel> Channel;  // Array of channel objects
   std::vector<AudioChannel> Shadow;   // Array of shadow channels for oversampling
   std::vector<AudioCommand> Commands; // Buffered commands.
   std::shared_ptr<AudioEffectChain> Effects;
   std::vector<float> ScratchBuffer;
   int Tempo;        // Tracker tempo in beats per minute (24 ticks per beat)
   SAMPLE MixLeft;    // Amount of mix elements left before the next command-update occurs

   ChannelSet() {
      clear();
   }

   ChannelSet(ChannelSet and) noexcept = default;
   ChannelSet &operator=(ChannelSet and) noexcept = default;
   ChannelSet(const ChannelSet &) = delete;
   ChannelSet &operator=(const ChannelSet &) = delete;

   ~ChannelSet() {
      clear();
   }

   SAMPLE TickFrames(int OutputRate) const {
      if (Tempo <= 0 or OutputRate <= 0) return SAMPLE(0);
      const auto frames = (int64_t(OutputRate) * 5 / (int64_t(Tempo) * 2) + 1) & ~int64_t(1);
      return SAMPLE(std::max(int64_t(2), frames));
   }

   void clear() {
      Channel.clear();
      Shadow.clear();
      Commands.clear();
      Effects.reset();
      ScratchBuffer.clear();
      Tempo = 0;
      MixLeft    = SAMPLE(0);
   }
};

//********************************************************************************************************************

struct VolumeCtl {
   std::string Name;     // Name of the mixer
   VCF Flags;            // Special flags identifying the mixer's attributes.
   std::vector<float> Channels; // A variable length array of channel volumes.

   VolumeCtl() {
      Flags = VCF::NIL;
      Channels = { -1 }; // A -1 value leaves the current system volume as-is.
   }

   VolumeCtl(std::string pName, VCF pFlags = VCF::NIL, double pVolume = -1) {
      Name = pName;
      Flags = pFlags;
      Channels = { (float)pVolume };
   }
};

//********************************************************************************************************************

struct MixTimer {
   int64_t Time;
   int  SampleHandle;
   MixTimer(int64_t pTime, int pHandle) : Time(pTime), SampleHandle(pHandle) { }
};

//********************************************************************************************************************

class extAudio : public objAudio {
   public:
   // Public entry points and the mixer share this lock.  The worker never takes the Core object lock.
   std::shared_ptr<std::recursive_mutex> MixerLock = std::make_shared<std::recursive_mutex>();
   std::recursive_mutex &MixerMutex = *MixerLock;
   std::shared_ptr<AudioEffectChain> GlobalEffects = std::make_shared<AudioEffectChain>(MixerLock);
   int StreamBufferMs = 1000; // Source prefetch duration; independent of output latency.
   AudioCompletions BatchCompletions;
   TIMER BatchTimer = nullptr;
   bool DispatchingBatches = false;
   std::vector<ChannelSet> Sets; // Channels are grouped into sets.  Index 0 is a dummy entry.
   std::vector<AudioSample> Samples; // Buffered samples loaded into the audio object.
   std::vector<VolumeCtl> Volumes;
   std::vector<MixTimer> MixTimers;
   AudioConfig MixConfig;
   std::vector<float> MixBuffer;
   APTR  TaskRemovedHandle;
   APTR  UserLoginHandle;
   #ifdef ALSA_ENABLED
      pthread_t Worker{};
      int WakeFD = -1;
      int NotifyFD = -1;
      std::vector<pollfd> PollDescriptors;
      AudioWorkerStats WorkerStats;
      snd_pcm_t *Handle;
      snd_mixer_t *MixHandle;
      snd_output_t *sndlog;
   #endif
   #ifdef _WIN32
      WasapiStream *RenderStream = nullptr;
      unsigned QueuedFrames = 0;
      int64_t QueuedAt = 0;
      unsigned TailFrames = 0;
      bool Reopening = false;
      unsigned ReopenAttempts = 0;
      int64_t ReopenAt = 0;
      int64_t WorkerStartedAt = 0;
      uint64_t MixerContentions = 0;
   #endif
   #ifdef AUDIO_WORKER
      bool WorkerStarted = false;
      std::atomic<bool> StopWorker{true};
      std::atomic<int> WorkerError{0};
      std::array<AudioCommand, 1024> PendingCommands;
      size_t PendingCount = 0;
      struct Notification {
         int Sample;
         int Channel;
         uint64_t SampleGeneration;
         uint64_t PlaybackGeneration;
         int64_t Due;
      };
      std::array<Notification, 4096> Notifications;
      size_t NotificationCount = 0;
      uint64_t Starvations = 0;
      float FilterHistory[4] = {};
      uint64_t PeriodFrames = 0;
      uint64_t BufferFrames = 0;
      unsigned FrameBytes = 0;
      std::vector<uint8_t> AudioBuffer;
   #endif
   double  MasterVolume;
   TIMER   Timer;
   SAMPLE  MixElements;
   int     MaxChannels;    // Recommended maximum mixing channels for Sound class
   std::string Device;
   int8_t  DriverBitSize;  // Target sample bit size; accounts for stereo channel
   bool    Stereo;
   bool    Mute;
   bool    Initialising;

   inline struct AudioChannel * GetChannel(int Handle) {
      const auto index = unsigned(Handle) >> 16;
      if (index >= Sets.size() or size_t(Handle & 0xffff) >= Sets[index].Channel.size()) return nullptr;
      return &this->Sets[index].Channel[Handle & 0xffff];
   }

   inline struct AudioChannel * GetShadow(int Handle) {
      return &this->Sets[Handle>>16].Shadow[Handle & 0xffff];
   }

   inline double MixerLag();
   void reset_lag() { mixerLag = 0; }

   inline void finish(AudioChannel &Channel, bool Notify);

   extAudio(objMetaClass *ClassPtr, OBJECTID ObjectID);
   ~extAudio();

   private:
      double mixerLag = 0;
};

//********************************************************************************************************************

class extSound : public objSound {
   public:
   FUNCTION OnStop;
   std::array<uint8_t,32> Header;
   ankerl::unordered_dense::map<std::string, std::string> Tags;
   std::unique_ptr<objFile, DeleteObject<objFile>> File;
   std::string Path;
   int   Format;             // The format of the sound data
   int   DataOffset;         // Start of raw audio data within the source file
   int   Note;               // Note to play back (e.g. C, C#, G...)
   std::string NoteString;
   bool  FeedingStream = false;
   bool  Active;             // True once the sound is registered with the audio driver or mixer.

   extSound(objMetaClass *ClassPtr, OBJECTID ObjectID) : objSound(ClassPtr, ObjectID) {
      Compression = 50;     // 50% compression by default
      Volume      = 1.0;    // Playback at 100% volume level
      Pan         = 0;
      Playback    = 0;
      Note        = NOTE_C; // Standard pitch
      Stream      = STREAM::SMART;
   }

   ~extSound();
};

//********************************************************************************************************************

static void execute_audio_command(extAudio *, const AudioCommand &);
static void cancel_audio_batches(extAudio *, unsigned = 0);
static ERR audio_batch_timer(extAudio *, int64_t, int64_t);

#ifdef AUDIO_WORKER
static thread_local bool glAudioWorker = false;

static void wake_audio(extAudio *Self)
{
#ifdef _WIN32
   wasapi_wake(Self->RenderStream);
#else
   if (Self->WakeFD >= 0) {
      uint64_t value = 1;
      (void)write(Self->WakeFD, &value, sizeof(value));
   }
#endif
}

static void notify_audio(extAudio *Self)
{
#ifdef _WIN32
   wasapi_notify(Self->RenderStream);
#else
   if (Self->NotifyFD >= 0) {
      uint64_t value = 1;
      (void)write(Self->NotifyFD, &value, sizeof(value));
   }
#endif
}

static ERR start_audio_worker(extAudio *);
static void stop_audio_worker(extAudio *);
static void request_stream(extAudio *, AudioSample &);
#ifdef _WIN32
static void reopen_windows_audio(extAudio *);
#endif
#endif

//********************************************************************************************************************
// Logging may acquire Core locks or perform I/O, so it is disabled on the PCM worker.

class AudioLog {
   std::optional<kt::Log> logger;
public:
   AudioLog(CSTRING Name) {
      #ifdef AUDIO_WORKER
      if (glAudioWorker) return;
      #endif
      logger.emplace(Name);
   }
   ERR warning(ERR Error) { if (logger) return logger->warning(Error); return Error; }
   template<class... Args> void warning(CSTRING Format, const Args &... Values) {
      if (logger) logger->warning(Format, Values...);
   }
   template<class... Args> void traceBranch(CSTRING Format, const Args &... Values) {
      if (logger) logger->traceBranch(Format, Values...);
   }
};
