
using namespace kt;

#include <variant>
#include <mutex>
#include <optional>
#include <atomic>
#include <deque>
#ifdef _WIN32
#include "wasapi.h"
#endif
#include <unordered_map>
#include <unordered_set>
#ifdef ALSA_ENABLED
#include <pthread.h>
#include <poll.h>
#include <sys/eventfd.h>
#endif
#include "audio_buffer.h"
#ifdef ALSA_ENABLED
#include "audio_worker.h"
#endif
#include "audio_format.h"
#include "mixer_dispatch.h"
#include "audio_completions.h"

#define MIX_INTERVAL 0.01

enum SAMPLE : int64_t {};
enum BYTELEN : int64_t {};

inline BYTELEN operator + (BYTELEN a, BYTELEN b) { return BYTELEN(int64_t(a) + int64_t(b)); }
inline BYTELEN &operator += (BYTELEN &a, BYTELEN b) { a = a + b; return a; }

inline SAMPLE &operator -= (SAMPLE &a, SAMPLE b) { a = SAMPLE(int64_t(a) - int64_t(b)); return a; }

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
   RELEASE,
   POSITION,
   PLAY,
   MUTE,
   SET_LENGTH,
   CONTINUE,
   PAUSE
};

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
static const uint16_t WAVE_EXTENSIBLE = 0xfffe; // WAVE_FORMAT_EXTENSIBLE; the sub-format GUID holds the format tag.


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
   PcmFormat Format;      // Registered source format, copied from the client's descriptor
   LOOP     LoopMode;     // Loop mode (single, double)
   PCM      SampleType;   // Mixer representation of the source format
   int      FrameBytes = 1; // Bytes per source frame
   bool     Swap = false; // Streams only.  Callback data must be byte-swapped to native order.
   LTYPE    Loop1Type;    // First loop type (unidirectional, bidirectional)
   LTYPE    Loop2Type;    // Second loop type (unidirectional, bidirectional)
   #ifdef AUDIO_WORKER
   uint64_t Generation = 0;
   uint64_t DeferredStops = 0;
   int64_t DeferredStopDue = 0;
   AudioRingCursor Ring;
   int64_t SourceOffset = 0;
   int64_t RetryAt = 0;
   bool Refilling = false;
   bool SourceSeek = true;
   bool RefillPending = false;
   bool Prefilled = false;
   bool Starved = false;
   bool EndOfSource = false;
   #endif
   bool     StreamLengthKnown = false; // True when StreamLength is a finite source boundary.
   bool     Released = false; // Streams only.  Playback has left the stream loop; reset by MixPlay().
   bool     Stream;       // True if this is a stream

   // Stream loops use Loop2; a release suspends it for the current playback without altering the configuration.
   inline bool streamLoops() const { return (Loop2Type != LTYPE::NIL) and (!Released); }

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
      PlayPos = BYTELEN(0);
      BufferedLength = BYTELEN(0);
      StreamLengthKnown = false;
      Released     = false;
      Format       = PcmFormat();
      SampleType   = PCM::NIL;
      FrameBytes   = 1;
      Swap         = false;
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
   std::variant<double,int,int64_t,bool> Data; // Special data related to the command ID

   AudioCommand(CMD pCommandID, int pHandle) :
      CommandID(pCommandID), Handle(pHandle), Data(double(0)) { }

   AudioCommand(CMD pCommandID, int pHandle, double pData) :
      CommandID(pCommandID), Handle(pHandle), Data(pData) { }

   AudioCommand(CMD pCommandID, int pHandle, int pData) :
      CommandID(pCommandID), Handle(pHandle), Data(pData) { }

   AudioCommand(CMD pCommandID, int pHandle, int64_t pData) :
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
   int64_t  Position;       // Current playing/mixing frame position within Sample.
   int      Frequency;      // Playback frequency
   int      PositionLow;    // Playing position, lower bits
   int8_t   Priority;       // Priority of the sound that has been assigned to this channel
   CHS      State;          // Channel state
   CHS      ResumeState;    // PLAYING or RELEASED; restored by MixContinue() while Paused
   int8_t   LoopIndex;      // The current active loop (either 0, 1 or 2)
   bool     Paused;         // State is STOPPED by MixPause(); cleared by any terminal transition

   bool active() const {
      return Frequency ? true : false;
   }

   inline bool isStopped() const {
      return ((State IS CHS::STOPPED) or (State IS CHS::FINISHED));
   }
};

class extAudioEffect;

#include "audio_effect_params.h"

//********************************************************************************************************************
// Separate from Object: a vtable must never precede the framework's fixed object header.

enum class AudioTail { NONE, FINITE, INDEFINITE };

// Prepared on the control thread; publication only swaps storage. Retired storage is freed after unlocking.
class AudioEffectConfiguration {
public:
   virtual ~AudioEffectConfiguration() = default;
   virtual void publish() = 0;
   virtual int64_t latency() const = 0;
};

class AudioEffectProcessor {
public:
   virtual ~AudioEffectProcessor() = default;
   // Buffers contain normalised floating-point audio: magnitude 1.0 is 0 dBFS, with finite headroom above unity.
   // process() must not dereference the owning effect object.  A container fading out a removed branch keeps
   // calling process() on that branch's processors after their effects have been disconnected and possibly freed.
   virtual void process(float *Buffer, int Frames) = 0;
   // Notify observers of output frames that bypass processing or belong to an idle chain.
   virtual void skip(int Frames) { }
   virtual void reset() = 0;
   virtual bool pending() const { return false; }
   // Queries are bounded and allocation-free. FINITE bounds include buffered output and serial internal stages.
   // A processor with NONE may still have buffered output (for example a lookahead limiter).
   virtual AudioTail tail() const { return AudioTail::NONE; }
   virtual uint64_t tail_frames() const { return 0; }
   // Typical frames, excluding latency(), for output from input entering now to decay by 60 dB.  Unlike the
   // conservative tail_frames() bound, this estimates the audible tail of one voice among others on a shared chain.
   virtual uint64_t decay_estimate() const { return 0; }
   virtual int64_t latency() const { return 0; }
   virtual double gain_reduction() const { return 0; }
   // Values of AudioMeterSource::PROCESSOR meters, read under the mixer lock at the end of every meter interval.
   // Value is the index declared by the output template, and Channel is the channel index within the processing layout
   // for a channel-scoped meter, or -1.  Set Floor if the value is at the floor of its range.  Constant-time only.
   virtual double meter_value(int Value, int Channel, bool &Floor) const { return 0; }
   // Called under the mixer lock once the values of an interval have been read, and whenever the meter is reset, so
   // that measurements over the meter interval can restart.  May run on the render thread, so it must not allocate.
   virtual void meter_reset() { }
   // Called off the render thread and outside the mixer mutex. Read only immutable configuration here;
   // prepared storage must not reference the owning framework object, whose lifetime may end during preparation.
   virtual ERR prepare(int Rate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) {
      return ERR::Okay;
   }
};

// A branch chain is hosted by a container effect such as AudioSplitter.  It shares the mixer mutex and generation of
// the container's chain and is configured together with it.  DrainFrames, State and Truncated are only used by
// top-level chains, which control draining for everything that they host.

struct AudioEffectChain {
   std::shared_ptr<std::recursive_mutex> Mutex;
   std::vector<extAudioEffect *> Effects;
   uint64_t NextSequence = 0;
   std::shared_ptr<uint64_t> Generation = std::make_shared<uint64_t>(1);
   uint64_t DrainFrames = 0;
   ADS State = ADS::IDLE;
   bool Truncated = false;
   int Rate = 0;
   bool Stereo = false;
   std::vector<int> Layout; // Committed processing layout; empty until configured
   uint64_t FormatGeneration = 0; // Output configuration generation at the last commit
   OBJECTID Container = 0;  // Branch chains only: the hosting container effect
   int Branch = -1;         // Branch chains only: the current branch index within the container
   double LatencyBudget = 0; // Branch chains only: the container's latency budget in milliseconds
   bool pending() const;
   void reset();
   ERR latency(int64_t &Frames) const;
   uint64_t tail_bound() const;
   uint64_t decay_estimate() const;

   explicit AudioEffectChain(std::shared_ptr<std::recursive_mutex> Lock) : Mutex(std::move(Lock)) { }
   ~AudioEffectChain();
};

// A latency budget in milliseconds as whole output frames, rounded up.

inline int64_t budget_frames(double Milliseconds, int Rate)
{
   if ((Rate <= 0) or (Milliseconds <= 0)) return 0;
   return int64_t(std::ceil(Milliseconds * double(Rate) / 1000.0 - 1e-9));
}

// Snapshot storage is sized on the control thread when the meter layout changes.  The render thread only overwrites
// existing elements, so publication never allocates.

struct AudioMeterSnapshot {
   std::vector<double> Values;
   std::vector<int> ValueFlags; // AMV flags, one element per value
   uint64_t Sequence = 0, Generation = 0, Position = 0;
   int Interval = 0;
   AMF Flags = AMF::NIL;
};

class extAudioEffect : public objAudioEffect {
public:
   // The weak reference allows Audio or a channel set to disappear before an externally owned effect.
   std::weak_ptr<AudioEffectChain> Chain;
   uint64_t Sequence = 0;
   bool ResetPending = true;
   // Set by subclass constructors that publish parameters.  Static storage; never freed.
   const AudioEffectSchema *Schema = nullptr;
   // Changes staged by SetKey(), InsertEntry() and RemoveEntry() after initialisation, pending Flush().
   std::unique_ptr<AudioParamState> Pending;

   std::vector<int> Layout;        // Processing layout.  Input and output layouts are always identical.
   uint64_t FormatGeneration = 0;  // Output configuration generation of the committed layout
   bool FormatCommitted = false;   // True once a processing layout has been committed by device activation
   bool Stereo = false;            // Derived from Layout for the stereo-only DSP implementations

   // Container effects only: the branch chains in branch order.  Read and written under the mixer mutex.
   std::vector<std::shared_ptr<AudioEffectChain>> Branches;

   std::vector<AudioMeterDesc> Meters; // Current meter layout
   std::vector<double> InputPeaks, OutputPeaks; // Per-channel accumulators for the current interval
   double Reduction = 0;

   extAudioEffect(objMetaClass *ClassPtr, OBJECTID ObjectID) : objAudioEffect(ClassPtr, ObjectID) {
      AudioID = ParentID = 0;
      Channel = Order = OutputRate = Branch = 0;
      Flags = AEF::NIL;
   }
   ~extAudioEffect();
   void detach();
   // Subclasses publish a fully configured processor as their final Init step.  No subsequent unsynchronised writes.
   ERR set_processor(std::unique_ptr<AudioEffectProcessor> Processor);
   void process(float *Buffer, int Frames);
   void skip(int Frames);
   bool pending() const;
   int64_t latency() const;
   void reset_meter(uint64_t Generation);
   void idle();
   void set_layout(std::span<const int> Layout);
   uint64_t meter_generation(const AudioEffectChain *Chain) const;
   void read_meter(const AudioEffectChain *Chain, MeterReading &Reading) const;
   AudioMeterSnapshot Meter;
   uint64_t MeterPosition = 0;
   int MeterFrames = 0;
   int64_t CommittedLatency = 0;
   void publish_meter(AMF Flags);
   std::shared_ptr<AudioEffectProcessor> processor;


};

// Container effects host branch chains.  A container cannot be attached to a branch.

inline bool is_effect_container(CLASSID ClassID)
{
   return ClassID IS CLASSID::AUDIOSPLITTER;
}

static void process_effects(AudioEffectChain &, float *, int);
#ifdef UNIT_TESTS
static ERR configure_effects(AudioEffectChain &, int, std::span<const int>, uint64_t = 0);
#endif
static void render_effects(AudioEffectChain &, float *, int, int, uint64_t, bool = false, uint64_t = 0,
   bool = false);
static bool effects_pending(const AudioEffectChain &);

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

// A stop whose path has effects waits for their tail before its due time is set; see resolve_drained_stops().

struct Notification {
   int Sample;
   int Channel;
   uint64_t SampleGeneration;
   uint64_t PlaybackGeneration;
   int64_t Due;              // PreciseTime() at which the callback is delivered, once not awaiting a drain
   int64_t Audible = 0;      // PreciseTime() at which the voice's final source frame is expected to be heard
   int64_t Deadline = 0;     // Latest PreciseTime() to wait for a drain
   bool AwaitDrain = false;  // True while the voice's effect path is still producing its tail
};

//********************************************************************************************************************

class extAudio : public objAudio {
   public:
   // Public entry points and the mixer share this lock.  The worker never takes the Core object lock.  Take it only for
   // state that the mixer or effect chains also touch.  Main-thread-only state (Volumes, Device, init-only fields) is
   // serialised by the object lock, and main-thread reads of fields that only the main thread writes need no lock.

   std::shared_ptr<std::recursive_mutex> MixerLock = std::make_shared<std::recursive_mutex>();
   std::recursive_mutex &MixerMutex = *MixerLock;
   std::shared_ptr<AudioEffectChain> GlobalEffects = std::make_shared<AudioEffectChain>(MixerLock);

   AudioCompletions BatchCompletions; // Pending batch callbacks; the worker completes and the client thread dispatches.
   AudioConfig MixConfig; // Stereo/oversampling configuration used to select the mixing routine.
   std::vector<int> OutputLayout = { int(SPK::FRONT_LEFT), int(SPK::FRONT_RIGHT) }; // Requested output layout
   std::vector<int> CommittedLayout; // Processing layout of the last successful activation; empty if none
   int CommittedRate = 0; // Sample rate of the last successful activation, independent of device negotiation
   uint64_t OutputGeneration = 0; // Incremented whenever a processing configuration is committed
   bool LayoutExplicit = false; // True if OutputLayout was set by the client or configuration file
   bool OutputActive = false;  // True between a successful activation and deactivation
   std::vector<ChannelSet> Sets; // Channels are grouped into sets.  Index 0 is a dummy entry.
   std::vector<AudioSample> Samples; // Buffered samples loaded into the audio object.
   std::vector<VolumeCtl> Volumes; // Mixer volume controls.  Index 0 is the master control.
   std::vector<float> MixBuffer; // Floating-point mixing window, converted to the driver format on output.
   TIMER  BatchTimer = nullptr; // Client timer that dispatches BatchCompletions; survives device shutdown.
   double MaxDrain = 30; // Deadline in seconds for automatic DSP effect drains.  Locked once effects are configured.
   int    SourceFrames = 0; // Last real source frame mixed in the current window, including silent samples.
   int    StreamBufferMs = 1000; // Source prefetch duration; independent of output latency.
   bool   EffectConfigured = false; // True while effect chains are configured for the active output rate/layout.
   bool   DispatchingBatches = false; // Re-entrancy guard for audio_batch_timer() while callbacks are running.

   #ifdef ALSA_ENABLED
      pthread_t Worker{};
      int WakeFD = -1;   // eventfd that wakes the worker's poll() when commands are queued or on shutdown.
      int NotifyFD = -1; // eventfd that signals the client thread when the worker has pending notifications.
      std::vector<pollfd> PollDescriptors; // PCM poll descriptors, with WakeFD as the last entry.
      AudioWorkerStats WorkerStats;
      snd_pcm_t *Handle;
      snd_mixer_t *MixHandle;
      snd_output_t *sndlog;
   #endif

   #ifdef _WIN32
      WasapiStream *RenderStream = nullptr;
      unsigned QueuedFrames = 0; // Frames queued in the WASAPI buffer at QueuedAt; used to estimate mixer lag.
      int64_t  QueuedAt = 0;
      int64_t  ReopenAt = 0; // Earliest PreciseTime() at which the next reopen may be attempted.
      int64_t  WorkerStartedAt = 0;
      uint64_t MixerContentions = 0; // Render packets silenced because MixerMutex was busy.  Diagnostic.
      unsigned ReopenAttempts = 0; // Consecutive reopen attempts; reset after 5 seconds of stable running.
      bool     Reopening = false; // True while a failed render stream is awaiting reopen.
   #endif

   #ifdef AUDIO_WORKER
      bool WorkerStarted = false;
      std::atomic<bool> StopWorker{true};
      std::atomic<int> WorkerError{0}; // Last fatal device error raised by the worker; consumed by the client thread.
      std::array<AudioCommand, 1024> PendingCommands; // Commands queued for execution on the worker thread.
      size_t PendingCount = 0;

      std::array<Notification, 256> Notifications; // Sample completion events awaiting client-side dispatch.
      size_t   NotificationCount = 0;
      // Client-owned tail waits are transferred out of Notifications so they cannot exhaust the worker's fixed queue.
      std::deque<Notification> DrainingNotifications;
      size_t DrainingCursor = 0;
      uint64_t Starvations = 0;  // Count of streamed samples that ran out of buffered data.
      uint64_t PeriodFrames = 0; // Frames per device period (ALSA).
      uint64_t BufferFrames = 0; // Total frames in the device buffer.
      float    FilterHistory[4] = {}; // Low/high-pass filter state: left d1, d2, then right d1, d2.
      unsigned FrameBytes = 0; // Bytes per frame in the device output format.
      std::vector<uint8_t> AudioBuffer; // One period of device-format output awaiting write.
   #endif

   double  MasterVolume;   // Master output volume applied during mixing.
   TIMER   Timer;          // Client timer that dispatches sample completion notifications.
   SAMPLE  MixElements;    // Capacity of MixBuffer in frames; the maximum mix window size.
   int     MaxChannels;    // Recommended maximum mixing channels for Sound class
   std::string Device;
   int8_t  DriverBitSize;  // Target sample bit size; accounts for stereo channel
   bool    Stereo;
   bool    Mute;
   bool    Initialising;   // Re-entrancy guard for Activate() while the device is being initialised.

   inline struct AudioChannel * GetChannel(int Handle) {
      const auto index = unsigned(Handle) >> 16;
      if (index >= Sets.size() or size_t(Handle & 0xffff) >= Sets[index].Channel.size()) return nullptr;
      return &this->Sets[index].Channel[Handle & 0xffff];
   }

   inline struct AudioChannel * GetShadow(int Handle) {
      return &this->Sets[Handle>>16].Shadow[Handle & 0xffff];
   }

   inline double MixerLag();
   inline double SubmitLag();
   void reset_lag() { mixerLag = 0; }

   inline void finish(AudioChannel &Channel, bool Notify);

   extAudio(objMetaClass *ClassPtr, OBJECTID ObjectID);
   ~extAudio();

   private:
      double mixerLag = 0;
};

//********************************************************************************************************************

static ERR commit_audio_output(extAudio *, std::span<const int>, ERR (*)(extAudio *));

class extSound : public objSound {
   public:
   FUNCTION OnStop;
   std::array<uint8_t,32> Header;
   ankerl::unordered_dense::map<std::string, std::string> Tags;
   std::unique_ptr<objFile, DeleteObject<objFile>> File;
   std::string Path;
   AudioFormat SourceFormat; // Configured (new sounds) or decoded source format; SampleFormat is NIL if unknown
   int   Format;             // The format of the sound data
   int   DataOffset;         // Start of raw audio data within the source file
   int   Note;               // Note to play back (e.g. C, C#, G...)
   std::string NoteString;
   int64_t StreamOffset = -1; // Byte offset of the next data that read_stream() will produce, or -1 if unknown
   bool  FeedingStream = false;
   bool  Active;             // True once the sound is registered with the audio driver or mixer.

   extSound(objMetaClass *ClassPtr, OBJECTID ObjectID) : objSound(ClassPtr, ObjectID) {
      Compression = 50;     // 50% compression by default
      Volume      = 1.0;    // Playback at 100% volume level
      Pan         = 0;
      Playback    = 0;
      Note        = NOTE_C; // Standard pitch
      Stream      = STREAM::SMART;
      SourceFormat.SampleRate   = 0;
      SourceFormat.SampleFormat = ASF::NIL; // Unknown until configured or decoded
      SourceFormat.Flags        = AFF::NIL;
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
