
using namespace kt;

#include <variant>
#include "mixer_dispatch.h"

#ifdef _WIN32
#define MIX_INTERVAL 0.1
#else
#define MIX_INTERVAL 0.01
#endif

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

// Audio channel commands

enum class CMD : int {
   START_SEQUENCE=1,
   END_SEQUENCE,
   SAMPLE,
   VOLUME,
   PAN,
   FREQUENCY,
   RATE,
   STOP,
   STOP_LOOPING,
   POSITION,
   PLAY,
   MUTE,
   SET_LENGTH,
   CONTINUE
};

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

typedef struct _GUID {
  unsigned long  Data1;
  unsigned short Data2;
  unsigned short Data3;
  unsigned char  Data4[8];
} GUID;

typedef struct WAVEFormat {
   int16_t Format;            // Type of WAVE data in the chunk: RAW or ADPCM
   int16_t Channels;          // Number of channels, 1=mono, 2=stereo
   int Frequency;             // Playback frequency
   int AvgBytesPerSecond;     // Channels * SamplesPerSecond * (BitsPerSample / 8)
   int16_t BlockAlign;        // Channels * (BitsPerSample / 8)
   int16_t BitsPerSample;     // Bits per sample
   int16_t ExtraLength;
} WAVEFORMATEX;

// Function to set mixing step for thread-safe operation
void set_mix_step(int step);

static const int16_t WAVE_RAW   = 0x0001;  // Uncompressed waveform data.
static const int16_t WAVE_ADPCM = 0x0002;  // ADPCM compressed waveform data.
static const int16_t WAVE_FLOAT = 0x0003;  // Uncompressed floating point waveform

const int DEFAULT_BUFFER_SIZE = 8096; // Measured in samples, not bytes

struct PlatformData { void *Void; };

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
   bool     Stream;       // True if this is a stream

   AudioSample() {
      Stream   = false;
      clear();
   }

   ~AudioSample() {
      clear();
   }

   void clear() {
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

struct AudioCommand {
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

struct AudioChannel {
   double   LVolume;        // Current left speaker volume after applying Pan (0 - 1.0)
   double   RVolume;        // Current right speaker volume after applying Pan (0 - 1.0)
   double   LVolumeTarget;  // Volume target when fading or ramping
   double   RVolumeTarget;  // Volume target when fading or ramping
   double   Volume;         // Playing volume (0 - 1.0)
   double   Pan;            // Pan value (-1.0 - 1.0)
   int64_t  EndTime;        // Anticipated end-time of playing the current sample, if OnStop is defined in the sample.
   int      SampleHandle;   // Sample index, direct lookup into extAudio->Samples
   CHF      Flags;          // Special flags
   int      Position;       // Current playing/mixing byte position within Sample.
   int      Frequency;      // Playback frequency
   int      PositionLow;    // Playing position, lower bits
   int8_t   Priority;       // Priority of the sound that has been assigned to this channel
   CHS      State;          // Channel state
   int8_t   LoopIndex;      // The current active loop (either 0, 1 or 2)
   bool     Buffering;

   bool active() {
      return Frequency ? true : false;
   }

   inline bool isStopped() {
      return ((State IS CHS::STOPPED) or (State IS CHS::FINISHED));
   }
};

struct ChannelSet {
   std::vector<AudioChannel> Channel;  // Array of channel objects
   std::vector<AudioChannel> Shadow;   // Array of shadow channels for oversampling
   std::vector<AudioCommand> Commands; // Buffered commands.
   int UpdateRate;   // Update rate, measured in milliseconds
   SAMPLE MixLeft;    // Amount of mix elements left before the next command-update occurs

   ChannelSet() {
      clear();
   }

   ~ChannelSet() {
      clear();
   }

   void clear() {
      Channel.clear();
      Shadow.clear();
      UpdateRate = 0;
      MixLeft    = SAMPLE(0);
   }
};

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

struct MixTimer {
   int64_t Time;
   int  SampleHandle;
   MixTimer(int64_t pTime, int pHandle) : Time(pTime), SampleHandle(pHandle) { }
};

class extAudio : public objAudio {
   public:
   std::vector<ChannelSet> Sets; // Channels are grouped into sets.  Index 0 is a dummy entry.
   std::vector<AudioSample> Samples; // Buffered samples loaded into the audio object.
   std::vector<VolumeCtl> Volumes;
   std::vector<MixTimer> MixTimers;
   AudioConfig MixConfig;
   std::vector<float> MixBuffer;
   APTR  TaskRemovedHandle;
   APTR  UserLoginHandle;
   #ifdef _WIN32
      uint8_t  PlatformData[128];  // Data area for holding platform/hardware specific information
   #endif
   #ifdef ALSA_ENABLED
      std::vector<uint8_t> AudioBuffer;
      snd_pcm_t    *Handle;
      snd_mixer_t  *MixHandle;
      snd_output_t *sndlog;
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
      return &this->Sets[Handle>>16].Channel[Handle & 0xffff];
   }

   inline struct AudioChannel * GetShadow(int Handle) {
      return &this->Sets[Handle>>16].Shadow[Handle & 0xffff];
   }

   inline SAMPLE MixLeft(int Value) {
      if (!Value) return SAMPLE(0);
      return SAMPLE((((100 * (int64_t)OutputRate) / (Value * 40)) + 1) & 0xfffffffe);
   }

   inline double MixerLag();

   inline void finish(AudioChannel &Channel, bool Notify);

   extAudio(objMetaClass *ClassPtr, OBJECTID ObjectID);
   ~extAudio();

   private:
      double mixerLag;
};

class extSound : public objSound {
   public:
   FUNCTION OnStop;
   std::array<uint8_t,32> Header;
   #ifdef _WIN32
   uint8_t  PlatformData[64];   // Data area for holding platform/hardware specific information
   #endif
   ankerl::unordered_dense::map<std::string, std::string> Tags;
   std::unique_ptr<objFile, DeleteObject<objFile>> File;
   std::string Path;
   TIMER StreamTimer;        // Timer to regularly trigger for provisioning streaming data.
   TIMER PlaybackTimer;      // Timer to trigger when playback ends.
   int   Format;             // The format of the sound data
   int   DataOffset;         // Start of raw audio data within the source file
   int   Note;               // Note to play back (e.g. C, C#, G...)
   std::string NoteString;
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
