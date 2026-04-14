/*********************************************************************************************************************

-CLASS-
Audio: Supports a machine's audio hardware and provides a client-server audio management service.

The Audio class provides a comprehensive audio service that works across multiple platforms and follows a client-server
design model. It serves as the foundation for all audio operations in Kōtuku framework, managing hardware resources,
sample mixing, and output buffering.

The Audio class supports 8/16/32 bit output in stereo or mono configurations, with advanced features including oversampling
for enhanced quality, intelligent streaming for large samples, multiple simultaneous audio channels, and command sequencing
for precise timing control. The internal floating-point mixer ensures high-quality audio processing regardless of the
target hardware bit depth.

For straightforward audio playback requirements, we recommend using the @Sound class interface, which provides a
simplified API whilst utilising the Audio class internally. Direct use of the Audio class is appropriate for
applications requiring precise mixer control, multiple simultaneous samples, or custom audio processing workflows.

Note: Support for audio recording is not currently available in this implementation.

-END-

*********************************************************************************************************************/

#include "mixer_dispatch.h"

#ifndef ALSA_ENABLED
static ERR init_audio(extAudio *Self)
{
   Self->BitDepth     = 16;
   Self->Stereo       = true;
   Self->MasterVolume = Self->Volumes[0].Channels[0];
   Self->Volumes[0].Flags |= VCF::MONO;
   for (int i=1; i < std::ssize(Self->Volumes[0].Channels); i++) Self->Volumes[0].Channels[i] = -1;
   if ((Self->Volumes[0].Flags & VCF::MUTE) != VCF::NIL) Self->Mute = true;
   else Self->Mute = false;

   return ERR::Okay;
}
#endif

inline double extAudio::MixerLag() {
   if (!mixerLag) {
      pf::Log log(__FUNCTION__);
      #ifdef _WIN32
         // Windows uses a split buffer technique, so the write cursor is always 1/2 a buffer ahead.
         mixerLag = MIX_INTERVAL + (double(MixElements>>1) / double(OutputRate));
      #elif ALSA_ENABLED
         mixerLag = MIX_INTERVAL + (AudioBufferSize / DriverBitSize) / double(OutputRate);
      #endif
      log.trace("Mixer lag: %.2f", mixerLag);
   }
   return mixerLag;
}

inline void extAudio::finish(AudioChannel &Channel, bool Notify) {
   if (!Channel.isStopped()) {
      Channel.State = CHS::FINISHED;
      if ((Channel.SampleHandle) and (Notify)) {
         #ifdef ALSA_ENABLED
            if ((Channel.EndTime) and (PreciseTime() < Channel.EndTime)) {
               this->MixTimers.emplace_back(Channel.EndTime, Channel.SampleHandle);
               Channel.EndTime = 0;
            }
            else audio_stopped_event(*this, Channel.SampleHandle);
         #else
            audio_stopped_event(*this, Channel.SampleHandle);
         #endif
      }
   }
   else Channel.State = CHS::FINISHED;
}

//********************************************************************************************************************
// The individual mixing functions and function pointer arrays have been replaced by the
// consolidated AudioMixer::dispatch_mix() function in mixer_dispatch.cpp

/*********************************************************************************************************************
-ACTION-
Activate: Enables access to the audio hardware and initialises the mixer.

An audio object must be activated before it can play audio samples. The activation process involves several critical steps:
hardware resource acquisition, mixer buffer allocation, and platform-specific driver initialisation.

Activation attempts to gain exclusive or shared access to the audio hardware device. On some platforms, this may fail if
another process has obtained an exclusive lock on the audio device. The specific behaviour depends on the underlying
audio system (ALSA on Linux, DirectSound on Windows).

If activation fails, the audio object remains in an inactive state but retains its configuration. Common failure
causes include hardware device unavailability, insufficient system resources, or driver compatibility issues.

All resources and device locks obtained during activation can be released through #Deactivate(). An inactive audio
object can perform configuration operations but cannot process audio samples.

-ERRORS-
Okay: Hardware activation completed successfully.
AllocMemory: Failed to allocate internal mixing buffers.
Failed: Hardware device unavailable or driver initialisation failed.

*********************************************************************************************************************/

static ERR AUDIO_Activate(extAudio *Self)
{
   pf::Log log;

   if (Self->Initialising) return ERR::Okay;

   log.branch();

   Self->Initialising = true;

   ERR error;
   if ((error = init_audio(Self)) != ERR::Okay) {
      Self->Initialising = false;
      return error;
   }

   // Calculate one mixing element size for the hardware driver (not our floating point mixer).

   if (Self->BitDepth IS 16) Self->DriverBitSize = sizeof(int16_t);
   else if (Self->BitDepth IS 24) Self->DriverBitSize = 3;
   else if (Self->BitDepth IS 32) Self->DriverBitSize = sizeof(float);
   else Self->DriverBitSize = sizeof(int8_t);

   if (Self->Stereo) Self->DriverBitSize *= 2;

   // Allocate a floating-point mixing buffer

   const int mixbitsize = Self->Stereo ? sizeof(float) * 2 : sizeof(float);

   Self->MixBufferSize = BYTELEN((int((mixbitsize * Self->OutputRate) * (MIX_INTERVAL * 1.5)) + 15) & (~15));
   Self->MixElements   = SAMPLE(Self->MixBufferSize / mixbitsize);

   if (AllocMemory(Self->MixBufferSize, MEM::DATA, &Self->MixBuffer) IS ERR::Okay) {
      // Configure the mixing system

      bool use_interpolation = (Self->Flags & ADF::OVER_SAMPLING) != ADF::NIL;
      Self->MixConfig = AudioConfig(Self->Stereo, use_interpolation);

      #ifdef _WIN32
         WAVEFORMATEX wave = {
            .Format            = int16_t((Self->BitDepth IS 32) ? WAVE_FLOAT : WAVE_RAW),
            .Channels          = int16_t(Self->Stereo ? 2 : 1),
            .Frequency         = 44100,
            .AvgBytesPerSecond = 44100 * Self->DriverBitSize,
            .BlockAlign        = Self->DriverBitSize,
            .BitsPerSample     = int16_t(Self->BitDepth),
            .ExtraLength       = 0
         };

         if (auto strerr = sndCreateBuffer(Self, &wave, Self->MixBufferSize, 0x7fffffff, (PlatformData *)Self->PlatformData, TRUE)) {
            log.warning(strerr);
            Self->Initialising = false;
            return ERR::Failed;
         }

         if (sndPlay((PlatformData *)Self->PlatformData, TRUE, 0)) {
            Self->Initialising = false;
            return log.warning(ERR::Failed);
         }
      #endif

      // Note: The audio feed is managed by audio_timer() and is not started until an audio playback command
      // is executed by the client.

      Self->Initialising = false;
      return ERR::Okay;
   }
   else {
      Self->Initialising = false;
      return log.warning(ERR::AllocMemory);
   }
}

/*********************************************************************************************************************

-METHOD-
AddSample: Adds a new sample to an audio object for channel-based playback.

Audio samples can be loaded into an Audio object for playback via the AddSample() or #AddStream() methods.  For small
samples under 512k we recommend AddSample(), while anything larger should be supported through #AddStream().

When adding a sample, it is essential to select the correct bit format for the sample data.  While it is important to
differentiate between simple attributes such as 8 or 16 bit data, mono or stereo format, you should also be aware of
whether or not the data is little or big endian, and if the sample data consists of signed or unsigned values.  Because
of the possible variations there are a number of sample formats, as illustrated in the following table:

!SFM

By default, all samples are assumed to be in little endian format, as supported by Intel CPU's.  If the data is in big
endian format, logical-or the SampleFormat value with `SFM::F_BIG_ENDIAN`.

It is also possible to supply loop information with the sample data.  This is achieved by configuring the !AudioLoop
structure:

!AudioLoop

The types that can be specified in the `LoopMode` field are:

!LOOP

The `Loop1Type` and `Loop2Type` fields alter the style of the loop.  These can be set to the following:

!LTYPE

-INPUT-
func OnStop: This optional callback function will be called when the stream stops playing.
int(SFM) SampleFormat: Indicates the format of the sample data that you are adding.
buf(ptr) Data: Points to the address of the sample data.
bufsize DataSize: Size of the sample data, in bytes.
struct(*AudioLoop) Loop: Optional sample loop information.
structsize LoopSize: Must be set to `sizeof(AudioLoop)` if `Loop` is defined.
&int Result: The resulting sample handle will be returned in this parameter.

-ERRORS-
Okay: Sample successfully added to the audio system.
Args: Invalid argument values provided.
NullArgs: Required parameters are null or missing.
AllocMemory: Failed to allocate enough memory to hold the sample data.
-END-

*********************************************************************************************************************/

ERR AUDIO_AddSample(extAudio *Self, struct snd::AddSample *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.branch("Data: %p, Length: %d", Args->Data, Args->DataSize);

   // Find an unused sample block.  If there is none, increase the size of the sample management area.

   int idx;
   for (idx=1; idx < std::ssize(Self->Samples); idx++) {
      if (!Self->Samples[idx].Data) break;
   }

   if (idx >= std::ssize(Self->Samples)) Self->Samples.resize(std::ssize(Self->Samples)+10);

   auto shift = sample_shift(Args->SampleFormat);

   auto &sample = Self->Samples[idx];
   sample.SampleType   = Args->SampleFormat;
   sample.SampleLength = SAMPLE(Args->DataSize >> shift);
   sample.OnStop       = Args->OnStop;

   if (auto loop = Args->Loop) {
      sample.LoopMode     = loop->LoopMode;
      sample.Loop1Start   = SAMPLE(loop->Loop1Start >> shift);
      sample.Loop1End     = SAMPLE(loop->Loop1End >> shift);
      sample.Loop1Type    = loop->Loop1Type;
      sample.Loop2Start   = SAMPLE(loop->Loop2Start >> shift);
      sample.Loop2End     = SAMPLE(loop->Loop2End >> shift);
      sample.Loop2Type    = loop->Loop2Type;
      // Eliminate zero-byte loops

      if (sample.Loop1Start IS sample.Loop1End) sample.Loop1Type = LTYPE::NIL;
      if (sample.Loop2Start IS sample.Loop2End) sample.Loop2Type = LTYPE::NIL;
   }
   else {
      sample.Loop1Type = LTYPE::NIL;
      sample.Loop2Type = LTYPE::NIL;
   }

   if ((sample.SampleType IS SFM::NIL) or (Args->DataSize <= 0) or (!Args->Data)) {
      sample.Data = nullptr;
   }
   else if (AllocMemory(Args->DataSize, MEM::DATA|MEM::NO_CLEAR, &sample.Data) IS ERR::Okay) {
      copymem(Args->Data, sample.Data, Args->DataSize);
   }
   else return log.warning(ERR::AllocMemory);

   Args->Result = idx;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
AddStream: Adds a new sample-stream to an Audio object for channel-based playback.

Use AddStream to load large sound samples to an Audio object, allowing it to play those samples on the client
machine without over-provisioning available resources.  For small samples under 256k consider using #AddSample()
instead.

The data source used for a stream will need to be provided by a client provided `Callback` function.  The prototype
is `INT callback(INT SampleHandle, INT Offset, UINT8 *Buffer, INT BufferSize)`.

The `Offset` reflects the retrieval point of the decoded data and is measured in bytes.  The `Buffer` and
`BufferSize` reflect the target for the decoded data.  The function must return the total number of bytes that were
written to the `Buffer`. If an error occurs, return zero.

When creating a new stream, pay attention to the audio format that is being used for the sample data.
It is important to differentiate between 8-bit, 16-bit, mono and stereo, but also be aware of whether or not the data
is little or big endian, and if the sample data consists of signed or unsigned values.  Because of the possible
variations there are a number of sample formats, as illustrated in the following table:

!SFM

By default, all samples are assumed to be in little endian format, as supported by Intel CPU's.  If the data is in big
endian format, logical-or the `SampleFormat` value with the flag `SFM::F_BIG_ENDIAN`.

It is also possible to supply loop information with the stream.  The Audio class supports a number of different looping
formats via the !AudioLoop structure:

!AudioLoop

There are three types of loop modes that can be specified in the `LoopMode` field:

!LOOP

The `Loop1Type` and `Loop2Type` fields normally determine the style of the loop, however only unidirectional looping is
currently supported for streams.  For that reason, set the type variables to either `LTYPE::NIL` or
`LTYPE::UNIDIRECTIONAL`.

-INPUT-
func Callback: This callback function must be able to return raw audio data for streaming.
func OnStop: This optional callback function will be called when the stream stops playing.
int(SFM) SampleFormat: Indicates the format of the sample data that you are adding.
int SampleLength: Total byte-length of the sample data that is being streamed.  May be set to zero if the length is infinite or unknown.
int PlayOffset: Offset the playing position by this byte index.
struct(*AudioLoop) Loop: Refers to sample loop information, or `NULL` if no loop is required.
structsize LoopSize: Must be set to `sizeof(AudioLoop)`.
&int Result: The resulting sample handle will be returned in this parameter.

-ERRORS-
Okay: Stream successfully configured and added to the audio system.
Args: Invalid argument values provided.
NullArgs: Required parameters are null or missing.
AllocMemory: Failed to allocate the stream buffer.
-END-

*********************************************************************************************************************/

static const int MAX_STREAM_BUFFER = 16 * 1024; // Max stream buffer length in bytes

static ERR AUDIO_AddStream(extAudio *Self, struct snd::AddStream *Args)
{
   pf::Log log;

   if ((!Args) or (Args->SampleFormat IS SFM::NIL)) return log.warning(ERR::NullArgs);
   if (Args->Callback.Type IS CALL::NIL) return log.warning(ERR::NullArgs);

   log.branch("Length: %d", Args->SampleLength);

   // Find an unused sample block.  If there is none, increase the size of the sample management area.

   int idx;
   for (idx=1; idx < std::ssize(Self->Samples); idx++) {
      if (!Self->Samples[idx].Data) break;
   }

   if (idx >= std::ssize(Self->Samples)) Self->Samples.resize(std::ssize(Self->Samples)+10);

   auto shift = sample_shift(Args->SampleFormat);

   int buffer_len;
   if (Args->SampleLength > 0) {
      buffer_len = Args->SampleLength / 2;
      if (buffer_len > MAX_STREAM_BUFFER) buffer_len = MAX_STREAM_BUFFER;
   }
   else buffer_len = MAX_STREAM_BUFFER; // Use the recommended amount of buffer space

   #ifdef ALSA_ENABLED
      if (buffer_len < Self->AudioBufferSize) log.warning("Warning: Buffer length of %d is less than audio buffer size of %d.", buffer_len, Self->AudioBufferSize);
   #endif

   // Setup the audio sample

   auto &sample = Self->Samples[idx];
   sample.SampleType   = Args->SampleFormat;
   sample.SampleLength = SAMPLE(buffer_len>>shift);
   sample.StreamLength = BYTELEN((Args->SampleLength > 0) ? Args->SampleLength : 0x7fffffff); // 'Infinite' stream length
   sample.Callback     = Args->Callback;
   sample.OnStop       = Args->OnStop;
   sample.Stream       = true;
   sample.PlayPos      = BYTELEN(Args->PlayOffset);

   sample.LoopMode     = LOOP::SINGLE;
   sample.Loop1Type    = LTYPE::UNIDIRECTIONAL;
   sample.Loop1Start   = SAMPLE(0);
   sample.Loop1End     = SAMPLE(buffer_len>>shift);

   if (Args->Loop) {
      sample.Loop2Type    = LTYPE::UNIDIRECTIONAL;
      sample.Loop2Start   = SAMPLE(Args->Loop->Loop1Start);
      sample.Loop2End     = SAMPLE(Args->Loop->Loop1End);
      sample.StreamLength = BYTELEN(sample.Loop2End<<shift);

      if (sample.Loop2Start IS sample.Loop2End) sample.Loop2Type = LTYPE::NIL;
   }

   if (AllocMemory(buffer_len, MEM::DATA|MEM::NO_CLEAR, &sample.Data) != ERR::Okay) {
      return ERR::AllocMemory;
   }

   Args->Result = idx;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Beep: Generates system alert tones through the platform's audio notification system.

Use Beep to emit a tone from the platform's audio notification system, typically through the PC speaker or
other system-level audio devices. This method is useful for generating simple alert sounds or notifications
without requiring a full audio sample or stream.

-INPUT-
int Pitch: The pitch of the beep in HZ.
int Duration: The duration of the beep in milliseconds.
int Volume: The volume of the beep, from 0 to 100.

-ERRORS-
Okay
NullArgs
NoSupport: PC speaker support is not available.

*********************************************************************************************************************/

static ERR AUDIO_Beep(extAudio *Self, struct snd::Beep *Args)
{
   if (!Args) return ERR::NullArgs;

#ifdef __linux__
   if (auto console = GetResource(RES::CONSOLE_FD); console != -1) {
      ioctl(console, KDMKTONE, ((1193190 / Args->Pitch) & 0xffff) | ((uint32_t)Args->Duration << 16));
      return ERR::Okay;
   }
#elif _WIN32
   if (sndBeep(Args->Pitch, Args->Duration)) {
      return ERR::Okay;
   }
#else
   #warning Platform requires support for Beep()
#endif
   return ERR::NoSupport;
}

/*********************************************************************************************************************

-METHOD-
CloseChannels: Frees audio channels that have been allocated for sample playback.

Use CloseChannels to destroy a group of channels that have previously been allocated through the #OpenChannels()
method.  Any audio commands buffered against the channels will be cleared instantly.  Any audio data that has already
been mixed into the output buffer can continue to play for 1 - 2 seconds.  If this is an issue then the volume should
be muted at the same time.

-INPUT-
int Handle: Must refer to a channel handle returned from the #OpenChannels() method.

-ERRORS-
Okay
NullArgs
Args

*********************************************************************************************************************/

static ERR AUDIO_CloseChannels(extAudio *Self, struct snd::CloseChannels *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.branch("Handle: $%.8x", Args->Handle);

   int index = Args->Handle>>16;
   if ((index < 0) or (index >= std::ssize(Self->Sets))) log.warning(ERR::Args);

   Self->Sets[index].clear(); // We can't erase because that would mess up other channel handles.
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Deactivate: Disables the audio mixer and returns device resources to the system.

Deactivating an audio object will switch off the mixer, clear the output buffer and return any allocated device
resources back to the host system.  The audio object will remain in a suspended state until it is reactivated.
-END-
*********************************************************************************************************************/

static ERR AUDIO_Deactivate(extAudio *Self)
{
   pf::Log log;

   log.branch();

   if (Self->Initialising) {
      log.msg("Audio is still in the process of initialisation.");
      return ERR::Okay;
   }

   acClear(Self);

#ifdef ALSA_ENABLED
   free_alsa(Self);
   //if (Self->MixHandle) { snd_mixer_close(Self->MixHandle); Self->MixHandle = NULL; }
   //if (Self->Handle) { snd_pcm_close(Self->Handle); Self->Handle = 0; }
#endif

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR AUDIO_Free(extAudio *Self)
{
   if ((Self->Flags & ADF::AUTO_SAVE) != ADF::NIL) Self->saveSettings();

   if (Self->Timer) { UpdateTimer(Self->Timer, 0); Self->Timer = nullptr; }

   acDeactivate(Self);

   if (Self->MixBuffer) { FreeResource(Self->MixBuffer); Self->MixBuffer = nullptr; }

#ifdef ALSA_ENABLED

   free_alsa(Self);

#elif _WIN32

   dsCloseDevice();

#endif

   Self->~extAudio();

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR AUDIO_Init(extAudio *Self)
{
   pf::Log log;

#ifdef _WIN32
   Self->OutputRate = 44100; // Mix rate is forced for direct sound
#endif

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR AUDIO_NewObject(extAudio *Self)
{
   pf::Log log;

   Self->OutputRate  = 44100;        // Rate for output to speakers
   Self->InputRate   = 44100;        // Input rate for recording
   Self->Quality     = 80;
   Self->BitDepth    = 16;
   Self->Flags       = ADF::OVER_SAMPLING|ADF::FILTER_HIGH|ADF::VOL_RAMPING|ADF::STEREO;
   Self->Periods     = 4;
   Self->PeriodSize  = 2048;
   Self->Device      = "default";
   Self->MaxChannels = 8;

   const SystemState *state = GetSystemState();
   if ((iequals(state->Platform, "Native")) or (iequals(state->Platform, "Linux"))) {
      Self->Flags |= ADF::SYSTEM_WIDE;
   }

   Self->Samples.reserve(32);

#ifdef __linux__
   Self->Volumes.resize(2);
   Self->Volumes[0].Name = "Master";
   for (int i=0; i < std::ssize(Self->Volumes[0].Channels); i++) Self->Volumes[0].Channels[i] = 1.0;

   Self->Volumes[1].Name = "PCM";
   for (int i=0; i < std::ssize(Self->Volumes[1].Channels); i++) Self->Volumes[1].Channels[i] = 1.0;
#else
   Self->Volumes.resize(1);
   Self->Volumes[0].Name = "Master";
   Self->Volumes[0].Channels[0] = 1.0;
   for (int i=1; i < std::ssize(Self->Volumes[0].Channels); i++) Self->Volumes[0].Channels[i] = -1;
#endif

   load_config(Self);

   return ERR::Okay;
}

static ERR AUDIO_NewPlacement(extAudio *Self)
{
   new (Self) extAudio;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
OpenChannels: Allocates audio channels that can be used for sample playback.

Use the OpenChannels method to open audio channels for sample playback.  Channels are allocated in sets with a size
range between 1 and 64.  Channel sets make it easier to segregate playback between users of the same audio object.

The resulting handle returned from this method is an integer consisting of two parts.  The upper word uniquely
identifies the channel set that has been provided to you, while the lower word is used to refer to specific channel
numbers.  If referring to a specific channel is required for a function, use the formula `Channel = Handle | ChannelNo`.

To destroy allocated channels, use the #CloseChannels() method.

-INPUT-
int Total: Total of channels to allocate.
&int Result: The resulting channel handle is returned in this parameter.

-ERRORS-
Okay
NullArgs
OutOfRange: The amount of requested channels or commands is outside of acceptable range.
AllocMemory: Memory for the audio channels could not be allocated.
-END-

*********************************************************************************************************************/

static ERR AUDIO_OpenChannels(extAudio *Self, struct snd::OpenChannels *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.branch("Total: %d", Args->Total);

   Args->Result = 0;
   if ((Args->Total < 0) or (Args->Total > 64)) {
      return log.warning(ERR::OutOfRange);
   }

   // Bear in mind that the +1 is for channel set 0 being a dummy entry.

   int index = std::ssize(Self->Sets) + 1;
   Self->Sets.resize(index+1);

   Self->Sets[index].Channel.resize(Args->Total);

   if ((Self->Flags & ADF::OVER_SAMPLING) != ADF::NIL) Self->Sets[index].Shadow.resize(Args->Total);
   else Self->Sets[index].Shadow.clear();

   Self->Sets[index].UpdateRate = 125;  // Default mixer update rate of 125ms
   Self->Sets[index].MixLeft    = Self->MixLeft(Self->Sets[index].UpdateRate);
   Self->Sets[index].Commands.reserve(32);

   Args->Result = index<<16;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
RemoveSample: Removes a sample from the global sample list and deallocates its resources.

Remove an allocated sample by calling the RemoveSample method.  Removed samples are permanently deleted from the
audio server and it is not possible to reallocate the sample against the same `Handle` value.

Sample handles can be reused by the API after being removed.  Clearing references to stale sample handles on the
client side is recommended.

-INPUT-
int Handle: The handle of the sample that requires removal.

-ERRORS-
Okay
NullArgs
OutOfRange: The provided sample handle is not within the valid range.
-END-

*********************************************************************************************************************/

static ERR AUDIO_RemoveSample(extAudio *Self, struct snd::RemoveSample *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.branch("Sample: %d", Args->Handle);

   if ((Args->Handle < 1) or (Args->Handle >= std::ssize(Self->Samples))) return log.warning(ERR::OutOfRange);

   Self->Samples[Args->Handle].clear();

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SaveSettings: Saves the current audio settings.
-END-
*********************************************************************************************************************/

static ERR AUDIO_SaveSettings(extAudio *Self)
{
   objFile::create file = { fl::Path("user:config/audio.cfg"), fl::Flags(FL::NEW|FL::WRITE) };

   if (file.ok()) return Self->saveToObject(*file);
   else return ERR::CreateFile;
}

/*********************************************************************************************************************
-ACTION-
SaveToObject: Saves the current audio settings to another object.
-END-
*********************************************************************************************************************/

static ERR AUDIO_SaveToObject(extAudio *Self, struct acSaveToObject *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Dest)) return log.warning(ERR::NullArgs);

   objConfig::create config = { };
   if (config.ok()) {
      config->write("AUDIO", "OutputRate", std::to_string(Self->OutputRate));
      config->write("AUDIO", "InputRate", std::to_string(Self->InputRate));
      config->write("AUDIO", "Quality", std::to_string(Self->Quality));
      config->write("AUDIO", "BitDepth", std::to_string(Self->BitDepth));
      config->write("AUDIO", "Periods", std::to_string(Self->Periods));
      config->write("AUDIO", "PeriodSize", std::to_string(Self->PeriodSize));

      if ((Self->Flags & ADF::STEREO) != ADF::NIL) config->write("AUDIO", "Stereo", "TRUE");
      else config->write("AUDIO", "Stereo", "FALSE");

#ifdef __linux__
      if (!Self->Device.empty()) config->write("AUDIO", "Device", Self->Device);
      else config->write("AUDIO", "Device", "default");

      if ((!Self->Volumes.empty()) and ((Self->Flags & ADF::SYSTEM_WIDE) != ADF::NIL)) {
         for (int i=0; i < std::ssize(Self->Volumes); i++) {
            std::ostringstream out;
            if ((Self->Volumes[i].Flags & VCF::MUTE) != VCF::NIL) out << "1,[";
            else out << "0,[";

            if ((Self->Volumes[i].Flags & VCF::MONO) != VCF::NIL) {
               out << Self->Volumes[i].Channels[0];
            }
            else for (int c=0; c < std::ssize(Self->Volumes[i].Channels); c++) {
               if (c > 0) out << ',';
               out << Self->Volumes[i].Channels[c];
            }
            out << ']';

            config->write("MIXER", Self->Volumes[i].Name.c_str(), out.str());
         }
      }
#if 0
   // Commented out because it prevents savetoobject being used by other tasks.

   snd_mixer_selem_id_t *sid;
   snd_mixer_elem_t *elem;
   int left, right;
   int pmin, pmax;
   int mute;
   int i;

   snd_mixer_selem_id_alloca(&sid);
   snd_mixer_selem_id_set_index(sid, 0);
   snd_mixer_selem_id_set_name(sid, Self->Volumes[i].Name);

   if ((elem = snd_mixer_find_selem(Self->MixHandle, sid))) {
      if (snd_mixer_selem_has_playback_volume(elem)) {
         snd_mixer_selem_get_playback_volume_range(elem, &pmin, &pmax);
         snd_mixer_selem_get_playback_volume(elem, 0, &left);
         snd_mixer_selem_get_playback_switch(elem, 0, &mute);
         if ((Self->Volumes[i].Flags & VCF::MONO) != VCF::NIL) right = left;
         else snd_mixer_selem_get_playback_volume(elem, 1, &right);
      }
      else if (snd_mixer_selem_has_capture_volume(elem)) {
         snd_mixer_selem_get_capture_volume_range(elem, &pmin, &pmax);
         snd_mixer_selem_get_capture_volume(elem, 0, &left);
         snd_mixer_selem_get_capture_switch(elem, 0, &mute);
         if ((Self->Volumes[i].Flags & VCF::MONO) != VCF::NIL) right = left;
         else snd_mixer_selem_get_capture_volume(elem, 1, &right);
      }
      else continue;

      if (pmin >= pmax) continue;

      std::ostringstream out;
      auto fleft = (double)left / (double)(pmax - pmin);
      auto fright = (double)right / (double)(pmax - pmin);
      out << fleft << ',' << fright << ',' << mute ? 0 : 1;

      config->write("MIXER", Self->Volumes[i].Name, out.str());
   }
#endif

#else
      if (!Self->Volumes.empty()) {
         std::string out(((Self->Volumes[0].Flags & VCF::MUTE) != VCF::NIL) ? "1,[" : "0,[");
         out.append(std::to_string(Self->Volumes[0].Channels[0]));
         out.append("]");
         config->write("MIXER", Self->Volumes[0].Name.c_str(), out);
      }
#endif

      config->saveToObject(Args->Dest);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetSampleLength: Sets the byte length of a streaming sample.

This function will update the byte length of a streaming `Sample`.  Although it is possible to manually stop a stream
at any point, setting the length is a preferable means to stop playback as it ensures complete accuracy when a
sample's output is buffered.

Setting a `Length` of `-1` indicates that the stream should be played indefinitely.

-INPUT-
int Sample: A sample handle from AddStream().
large Length: Byte length of the sample stream.

-ERRORS-
Okay
NullArgs
Args
Failed: Sample is not a stream.
-END-

*********************************************************************************************************************/

static ERR AUDIO_SetSampleLength(extAudio *Self, struct snd::SetSampleLength *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.msg("Sample: #%d, Length: %" PF64, Args->Sample, (long long)Args->Length);

   if ((Args->Sample < 0) or (Args->Sample >= std::ssize(Self->Samples))) return log.warning(ERR::Args);

   auto &sample = Self->Samples[Args->Sample];

   if (sample.Stream) {
      sample.StreamLength = BYTELEN(Args->Length);
      return ERR::Okay;
   }
   else return log.warning(ERR::Failed);
}

/*********************************************************************************************************************

-METHOD-
SetVolume: Sets the volume for input and output mixers.

To change volume and mixer levels, use the SetVolume method.  It is possible to make adjustments to any of the
available mixers and for different channels per mixer - for instance you may set different volumes for left and right
speakers.  Support is also provided for special options such as muting.

To set the volume for a mixer, use its index or set its name (to change the master volume, use a name of `Master`).

A target `Channel` such as the left `0` or right `1` speaker can be specified.  Set the `Channel` to `-1` if all
channels should be the same value.

The new mixer value is set in the `Volume` field.

Optional flags may be set as follows:

<types lookup="SVF"/>

-INPUT-
int Index: The index of the mixer that you want to set.
cstr Name: If the correct index number is unknown, the name of the mixer may be set here.
int(SVF) Flags: Optional flags.
int Channel: A specific channel to modify (e.g. `0` for left, `1` for right).  If `-1`, all channels are affected.
double Volume: The volume to set for the mixer, from 0 to 1.0.  If `-1`, the current volume values are retained.

-ERRORS-
Okay: The new volume was applied successfully.
Args
NullArgs
OutOfRange: The `Volume` or `Index` is out of the acceptable range.
-END-

*********************************************************************************************************************/

static ERR AUDIO_SetVolume(extAudio *Self, struct snd::SetVolume *Args)
{
   pf::Log log;

#ifdef ALSA_ENABLED

   int index;
   snd_mixer_selem_id_t *sid;
   snd_mixer_elem_t *elem;
   long pmin, pmax;

   if (!Args) return log.warning(ERR::NullArgs);
   if (((Args->Volume < 0) or (Args->Volume > 1.0)) and (Args->Volume != -1)) {
      return log.warning(ERR::OutOfRange);
   }
   if (Self->Volumes.empty()) return log.warning(ERR::NoSupport);
   if (!Self->MixHandle) return ERR::NotInitialised;

   // Determine what mixer we are going to adjust

   if (Args->Name) {
      for (index=0; index < std::ssize(Self->Volumes); index++) {
         if (iequals(Args->Name, Self->Volumes[index].Name)) break;
      }

      if (index IS (int)Self->Volumes.size()) return ERR::Search;
   }
   else {
      index = Args->Index;
      if ((index < 0) or (index >= (int)Self->Volumes.size())) return ERR::OutOfRange;
   }

   if (iequals("Master", Self->Volumes[index].Name)) {
      if (Args->Volume != -1) {
         Self->MasterVolume = Args->Volume;
      }

      if ((Args->Flags & SVF::UNMUTE) != SVF::NIL) {
         Self->Volumes[index].Flags &= ~VCF::MUTE;
         Self->Mute = false;
      }
      else if ((Args->Flags & SVF::MUTE) != SVF::NIL) {
         Self->Volumes[index].Flags |= VCF::MUTE;
         Self->Mute = true;
      }
   }

   // Apply the volume

   log.branch("%s: %.2f, Flags: $%.8x", Self->Volumes[index].Name.c_str(), Args->Volume, (int)Args->Flags);

   snd_mixer_selem_id_alloca(&sid);
   snd_mixer_selem_id_set_index(sid,0);
   snd_mixer_selem_id_set_name(sid, Self->Volumes[index].Name.c_str());
   if (!(elem = snd_mixer_find_selem(Self->MixHandle, sid))) {
      log.msg("Mixer \"%s\" not found.", Self->Volumes[index].Name.c_str());
      return ERR::Search;
   }

   if (Args->Volume >= 0) {
      if ((Self->Volumes[index].Flags & VCF::CAPTURE) != VCF::NIL) {
         snd_mixer_selem_get_capture_volume_range(elem, &pmin, &pmax);
      }
      else snd_mixer_selem_get_playback_volume_range(elem, &pmin, &pmax);

      pmax = pmax - 1; // -1 because the absolute maximum tends to produce distortion...

      double vol = Args->Volume;
      if (vol > 1.0) vol = 1.0;
      int lvol = int(double(pmin) + (double(pmax - pmin) * vol));

      if ((Self->Volumes[index].Flags & VCF::CAPTURE) != VCF::NIL) {
         snd_mixer_selem_set_capture_volume_all(elem, lvol);
      }
      else snd_mixer_selem_set_playback_volume_all(elem, lvol);

      if ((Self->Volumes[index].Flags & VCF::MONO) != VCF::NIL) {
         Self->Volumes[index].Channels[0] = vol;
      }
      else {
         if (Args->Channel IS -1) {
            for (int channel=0; channel < (int)Self->Volumes[index].Channels.size(); channel++) {
               if (Self->Volumes[index].Channels[channel] >= 0) {
                  Self->Volumes[index].Channels[channel] = vol;
               }
            }
         }
         else if ((Args->Channel >= 0) and (Args->Channel < int(Self->Volumes[index].Channels.size()))) {
            Self->Volumes[index].Channels[Args->Channel] = vol;
         }
      }
   }

   if ((Args->Flags & SVF::UNMUTE) != SVF::NIL) {
      if ((snd_mixer_selem_has_capture_switch(elem)) and (!snd_mixer_selem_has_playback_switch(elem))) {
         for (int chn=0; chn <= SND_MIXER_SCHN_LAST; chn++) {
            snd_mixer_selem_set_capture_switch(elem, (snd_mixer_selem_channel_id_t)chn, 1);
         }
      }
      else if (snd_mixer_selem_has_playback_switch(elem)) {
         for (int chn=0; chn <= SND_MIXER_SCHN_LAST; chn++) {
            snd_mixer_selem_set_playback_switch(elem, (snd_mixer_selem_channel_id_t)chn, 1);
         }
      }
      Self->Volumes[index].Flags &= ~VCF::MUTE;
   }
   else if ((Args->Flags & SVF::MUTE) != SVF::NIL) {
      if ((snd_mixer_selem_has_capture_switch(elem)) and (!snd_mixer_selem_has_playback_switch(elem))) {
         for (int chn=0; chn <= SND_MIXER_SCHN_LAST; chn++) {
            snd_mixer_selem_set_capture_switch(elem, (snd_mixer_selem_channel_id_t)chn, 0);
         }
      }
      else if (snd_mixer_selem_has_playback_switch(elem)) {
         for (int chn=0; chn <= SND_MIXER_SCHN_LAST; chn++) {
            snd_mixer_selem_set_playback_switch(elem, (snd_mixer_selem_channel_id_t)chn, 0);
         }
      }
      Self->Volumes[index].Flags |= VCF::MUTE;
   }

   EVENTID evid = GetEventID(EVG::AUDIO, "volume", Self->Volumes[index].Name.c_str());
   evVolume event_volume = { evid, Args->Volume, ((Self->Volumes[index].Flags & VCF::MUTE) != VCF::NIL) ? true : false };
   BroadcastEvent(&event_volume, sizeof(event_volume));
   return ERR::Okay;

#else

   int index;

   if (!Args) return log.warning(ERR::NullArgs);
   if (((Args->Volume < 0) or (Args->Volume > 1.0)) and (Args->Volume != -1)) {
      return log.warning(ERR::OutOfRange);
   }
   if (Self->Volumes.empty()) return log.warning(ERR::NoSupport);

   // Determine what mixer we are going to adjust

   if (Args->Name) {
      for (index=0; index < (int)Self->Volumes.size(); index++) {
         if (iequals(Args->Name, Self->Volumes[index].Name)) break;
      }

      if (index IS (int)Self->Volumes.size()) return ERR::Search;
   }
   else {
      index = Args->Index;
      if ((index < 0) or (index >= (int)Self->Volumes.size())) return ERR::OutOfRange;
   }

   if (iequals("Master", Self->Volumes[index].Name)) {
      if (Args->Volume != -1) {
         Self->MasterVolume = Args->Volume;
      }

      if ((Args->Flags & SVF::UNMUTE) != SVF::NIL) {
         Self->Volumes[index].Flags &= ~VCF::MUTE;
         Self->Mute = false;
      }
      else if ((Args->Flags & SVF::MUTE) != SVF::NIL) {
         Self->Volumes[index].Flags |= VCF::MUTE;
         Self->Mute = true;
      }
   }

   // Apply the volume

   log.branch("%s: %.2f, Flags: $%.8x", Self->Volumes[index].Name.c_str(), Args->Volume, (int)Args->Flags);

   if ((Args->Volume >= 0) and (Args->Volume <= 1.0)) {
      if ((Self->Volumes[index].Flags & VCF::MONO) != VCF::NIL) {
         Self->Volumes[index].Channels[0] = Args->Volume;
      }
      else {
         if (Args->Channel IS -1) {
            for (int channel=0; channel < (int)Self->Volumes[0].Channels.size(); channel++) {
               if (Self->Volumes[index].Channels[channel] >= 0) {
                  Self->Volumes[index].Channels[channel] = Args->Volume;
               }
            }
         }
         else if ((Args->Channel >= 0) and (Args->Channel < int(Self->Volumes[index].Channels.size()))) {
            Self->Volumes[index].Channels[Args->Channel] = Args->Volume;
         }
      }
   }

   if ((Args->Flags & SVF::UNMUTE) != SVF::NIL) Self->Volumes[index].Flags &= ~VCF::MUTE;
   else if ((Args->Flags & SVF::MUTE) != SVF::NIL) Self->Volumes[index].Flags |= VCF::MUTE;

   EVENTID evid = GetEventID(EVG::AUDIO, "volume", Self->Volumes[index].Name.c_str());
   evVolume event_volume = { evid, Args->Volume, ((Self->Volumes[index].Flags & VCF::MUTE) != VCF::NIL) ? true : false };
   BroadcastEvent(&event_volume, sizeof(event_volume));

   return ERR::Okay;

#endif
}

/*********************************************************************************************************************

-FIELD-
BitDepth: The bit depth affects the overall quality of audio input and output.

This field manages the bit depth for audio mixing and output.  Valid bit depths are `8`, `16` and `24`, with `16`
being the recommended value for CD quality playback.

*********************************************************************************************************************/

static ERR SET_BitDepth(extAudio *Self, int Value)
{
   if (Value IS 16) Self->BitDepth = 16;
   else if (Value IS 8) Self->BitDepth = 8;
   else if (Value IS 24) Self->BitDepth = 24;
   else return ERR::Failed;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Device: The name of the audio device used by this audio object.

A host platform may have multiple audio devices installed, but a given audio object can represent only one device
at a time.  A new audio object will always represent the default device initially.  Choose a different device by
setting the `Device` field to a valid alternative.

The default device can always be referenced with a name of `default`.

*********************************************************************************************************************/

static ERR GET_Device(extAudio *Self, CSTRING *Value)
{
   *Value = Self->Device.c_str();
   return ERR::Okay;
}

static ERR SET_Device(extAudio *Self, CSTRING Value)
{
   if ((!Value) or (!*Value)) Self->Device = "default";
   else {
      Self->Device = Value;
      std::transform(Self->Device.begin(), Self->Device.end(), Self->Device.begin(), ::tolower);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Flags: Special audio flags can be set here.
Lookup: ADF

The audio class supports a number of special flags that affect internal behaviour.  The following table illustrates the
publicly available flags:

!ADF

-FIELD-
InputRate: Determines the frequency to use when recording audio data.

The InputRate determines the frequency to use when recording audio data from a Line-In connection or microphone.  In
most cases, this value should be set to `44100` for CD quality audio.

The InputRate can only be set prior to initialisation, further attempts to set the field will be ignored.  On some
platforms, it may not be possible to set an InputRate that is different to the #OutputRate.  In such a case, the value
of the InputRate shall be ignored.

-FIELD-
MasterVolume: The master volume to use for audio playback.

The MasterVolume field controls the amount of volume applied to all of the audio channels.  Volume is expressed as
a value between `0` and `1.0`.

*********************************************************************************************************************/

static ERR GET_MasterVolume(extAudio *Self, double *Value)
{
   *Value = Self->MasterVolume;
   return ERR::Okay;
}

static ERR SET_MasterVolume(extAudio *Self, double Value)
{
   if (Value < 0) Value = 0;
   else if (Value > 1.0) Value = 1.0;

   return Self->setVolume(0, "Master", SVF::NIL, -1, Value);
}

/*********************************************************************************************************************

-FIELD-
MixerLag: Returns the lag time of the internal mixer, measured in seconds.

This field will return the worst-case value for latency imposed by the internal audio mixer.  The value is measured
in seconds and will differ between platforms and user configurations.

*********************************************************************************************************************/

static ERR GET_MixerLag(extAudio *Self, double *Value)
{
   *Value = Self->MixerLag();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Mute:  Mutes all audio output.

Audio output can be muted at any time by setting this value to `true`.  To restart audio output after muting, set the
field to `false`.  Muting does not disable the audio system, which is achieved by calling #Deactivate().

*********************************************************************************************************************/

static ERR GET_Mute(extAudio *Self, int *Value)
{
   *Value = FALSE;
   for (int i=0; i < std::ssize(Self->Volumes); i++) {
      if (iequals("Master", Self->Volumes[i].Name)) {
         if ((Self->Volumes[i].Flags & VCF::MUTE) != VCF::NIL) *Value = TRUE;
         break;
      }
   }
   return ERR::Okay;
}

static ERR SET_Mute(extAudio *Self, int Value)
{
   return Self->setVolume(0, "Master", Value ? SVF::MUTE : SVF::UNMUTE, -1, -1);
}

/*********************************************************************************************************************

-FIELD-
OutputRate: Determines the frequency to use for the output of audio data.

The OutputRate determines the frequency of the audio data that will be output to the audio speakers.  In most cases,
this value should be set to `44100` for CD quality audio.

The OutputRate can only be set prior to initialisation, further attempts to set the field will be ignored.

*********************************************************************************************************************/

static ERR SET_OutputRate(extAudio *Self, int Value)
{
   if (Value < 0) return ERR::OutOfRange;
   else if (Value > 44100) Self->OutputRate = 44100;
   else Self->OutputRate = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Periods: Defines the number of periods that make up the internal audio buffer.

The Periods field controls the segmentation of the internal audio buffer, directly affecting latency, performance, and
audio continuity. This setting is particularly relevant for ALSA-based systems where period-based buffering is fundamental
to audio driver operation.

The total audio buffer is divided into discrete periods, each representing a contiguous block of audio data. The audio
system processes data period by period, allowing for predictable latency characteristics and efficient interrupt handling.

<list type="bullet">
<li><b>Minimum</b>: 2 periods (provides double-buffering for basic audio continuity)</li>
<li><b>Maximum</b>: 16 periods (enables extensive buffering for demanding applications)</li>
<li><b>Recommended</b>: 4 periods (balances latency and reliability for most use cases)</li>
</list>

Fewer periods reduce overall system latency but increase the risk of audio dropouts if processing cannot keep pace with
audio consumption. More periods provide greater buffering security at the cost of increased latency.

*********************************************************************************************************************/

static ERR SET_Periods(extAudio *Self, int Value)
{
   Self->Periods = Value;
   if (Self->Periods < 2) Self->Periods = 2;
   else if (Self->Periods > 16) Self->Periods = 16;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
PeriodSize: Defines the byte size of each period allocated to the internal audio buffer.

The PeriodSize field determines the granularity of audio buffer management, affecting system responsiveness, processing
efficiency, and overall audio quality. This setting works in conjunction with the #Periods field to define the complete
buffering architecture.

<list type="bullet">
<li><b>Minimum</b>: 1024 bytes (1KB) - Enables very low latency but requires high-performance processing.</li>
<li><b>Maximum</b>: 16384 bytes (16KB) - Provides maximum buffering and processing time per period.</li>
<li><b>Recommended</b>: 2048 bytes (2KB) - Optimal balance for most applications and hardware configurations.</li>
</list>

Period latency (in seconds) = `PeriodSize ÷ (SampleRate × Channels × BytesPerSample)`

For example, with a 2048-byte period at 44100Hz stereo 16-bit `Latency = 2048 ÷ (44100 × 2 × 2) = 11.6ms per period`

The combination of PeriodSize and #Periods creates a multi-stage buffering system where audio data flows through
sequential periods, providing time for processing whilst maintaining continuous playback.

<header>Application Guidelines</header>

<list type="bullet">
<li><b>Real-time applications</b>: Use smaller periods (1-2KB) with fewer periods for minimal latency.</li>
<li><b>General applications</b>: Use medium periods (2-4KB) with 4 periods for reliable performance.</li>
<li><b>Background audio</b>: Use larger periods (8-16KB) with more periods for maximum efficiency.</li>
</list>

*********************************************************************************************************************/

static ERR SET_PeriodSize(extAudio *Self, int Value)
{
   Self->PeriodSize = Value;
   if (Self->PeriodSize < 1024) Self->PeriodSize = 1024;
   else if (Self->PeriodSize > 16384) Self->PeriodSize = 16384;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Quality: Determines the quality of the audio mixing.

The Quality field controls the precision and filtering applied during audio mixing operations. This setting automatically
configures multiple internal processing parameters to balance audio fidelity against computational overhead.

The value range spans from 0 (minimal processing) to 100 (maximum fidelity), with recommended settings between 70-80
for most applications. This range provides an optimal balance between audio clarity and system performance.

Setting the Quality value automatically adjusts the following processing flags:

<list type="bullet">
<li><b>Quality 0-9</b>: Minimal processing, no filtering or oversampling applied.</li>
<li><b>Quality 10-32</b>: Enables `ADF::FILTER_LOW` for basic output filtering.</li>
<li><b>Quality 33-65</b>: Activates `ADF::FILTER_HIGH` for enhanced frequency response.</li>
<li><b>Quality 66-100</b>: Implements `ADF::OVER_SAMPLING` and `ADF::FILTER_HIGH` for maximum fidelity.</li>
</list>

*********************************************************************************************************************/

static ERR SET_Quality(extAudio *Self, int Value)
{
   Self->Quality = Value;

   Self->Flags &= ~(ADF::FILTER_LOW|ADF::FILTER_HIGH|ADF::OVER_SAMPLING);

   if (Self->Quality < 10) return ERR::Okay;
   else if (Self->Quality < 33) Self->Flags |= ADF::FILTER_LOW;
   else if (Self->Quality < 66) Self->Flags |= ADF::FILTER_HIGH;
   else Self->Flags |= ADF::OVER_SAMPLING|ADF::FILTER_HIGH;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Stereo: Set to `true` for stereo output and `false` for mono output.

-END-

*********************************************************************************************************************/

static ERR GET_Stereo(extAudio *Self, int *Value)
{
   if ((Self->Flags & ADF::STEREO) != ADF::NIL) *Value = TRUE;
   else *Value = FALSE;
   return ERR::Okay;
}

static ERR SET_Stereo(extAudio *Self, int Value)
{
   if (Value IS TRUE) Self->Flags |= ADF::STEREO;
   else Self->Flags &= ~ADF::STEREO;
   return ERR::Okay;
}

//********************************************************************************************************************

static void load_config(extAudio *Self)
{
   pf::Log log(__FUNCTION__);

   // Attempt to get the user's preferred pointer settings from the user:config/pointer file.

   objConfig::create config = { fl::Path("user:config/audio.cfg") };

   if (config.ok()) {
      config->read("AUDIO", "OutputRate", Self->OutputRate);
      config->read("AUDIO", "InputRate", Self->InputRate);
      config->read("AUDIO", "Quality", Self->Quality);
      config->read("AUDIO", "BitDepth", Self->BitDepth);

      int value;
      if (config->read("AUDIO", "Periods", value) IS ERR::Okay) SET_Periods(Self, value);
      if (config->read("AUDIO", "PeriodSize", value) IS ERR::Okay) SET_PeriodSize(Self, value);
      if (config->read("AUDIO", "Device", Self->Device) != ERR::Okay) Self->Device = "default";

      std::string str;
      Self->Flags |= ADF::STEREO;
      if (config->read("AUDIO", "Stereo", str) IS ERR::Okay) {
         if (iequals("FALSE", str)) Self->Flags &= ~ADF::STEREO;
      }

      if ((Self->BitDepth != 8) and (Self->BitDepth != 16) and (Self->BitDepth != 24)) Self->BitDepth = 16;
      SET_Quality(Self, Self->Quality);

      // Find the mixer section, then load the mixer information

      ConfigGroups *groups;
      if (config->get(FID_Data, groups) IS ERR::Okay) {
         for (auto& [group, keys] : groups[0]) {
            if (iequals("MIXER", group)) {
               Self->Volumes.clear();
               Self->Volumes.resize(keys.size());

               int j = 0;
               for (auto& [k, v] : keys) {
                  Self->Volumes[j].Name = k;

                  CSTRING str = v.c_str();
                  if (std::stoi(v) IS 1) Self->Volumes[j].Flags |= VCF::MUTE;
                  while ((*str) and (*str != ',')) str++;
                  if (*str IS ',') str++;

                  int channel = 0;
                  if (*str IS '[') { // Read channel volumes
                     str++;
                     while ((*str) and (*str != ']')) {
                        Self->Volumes[j].Channels[channel] = strtol(str, nullptr, 0);
                        while ((*str) and (*str != ',') and (*str != ']')) str++;
                        if (*str IS ',') str++;
                        channel++;
                     }
                  }

                  while (channel < (int)Self->Volumes[j].Channels.size()) {
                     Self->Volumes[j].Channels[channel] = 0.75;
                     channel++;
                  }
                  j++;
               }
            }
            break;
         }
      }
   }
}

//********************************************************************************************************************

#include "audio_def.c"

static const FieldArray clAudioFields[] = {
   { "OutputRate",    FDF_INT|FDF_RI, nullptr, SET_OutputRate },
   { "InputRate",     FDF_INT|FDF_RI },
   { "Quality",       FDF_INT|FDF_RW,    nullptr, SET_Quality },
   { "Flags",         FDF_INTFLAGS|FDF_RI, nullptr, nullptr, &clAudioFlags },
   { "BitDepth",      FDF_INT|FDF_RI,    nullptr, SET_BitDepth },
   { "Periods",       FDF_INT|FDF_RI,    nullptr, SET_Periods },
   { "PeriodSize",    FDF_INT|FDF_RI,    nullptr, SET_PeriodSize },
   // VIRTUAL FIELDS
   { "Device",        FDF_STRING|FDF_RW,  GET_Device, SET_Device },
   { "MixerLag",      FDF_DOUBLE|FDF_R,   GET_MixerLag },
   { "MasterVolume",  FDF_DOUBLE|FDF_RW,  GET_MasterVolume, SET_MasterVolume },
   { "Mute",          FDF_INT|FDF_RW,    GET_Mute, SET_Mute },
   { "Stereo",        FDF_INT|FDF_RW,    GET_Stereo, SET_Stereo },
   END_FIELD
};

//********************************************************************************************************************

ERR add_audio_class(void)
{
   clAudio = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIO),
      fl::ClassVersion(1.0),
      fl::Name("Audio"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioActions),
      fl::Methods(clAudioMethods),
      fl::Fields(clAudioFields),
      fl::Size(sizeof(extAudio)),
      fl::Path(MOD_PATH));

   return clAudio ? ERR::Okay : ERR::AddClass;
}

void free_audio_class(void)
{
   if (clAudio) { FreeResource(clAudio); clAudio = nullptr; }
}
