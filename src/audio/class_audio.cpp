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

TODO: Add support for recording audio and live microphone streaming

*********************************************************************************************************************/

#include "mixer_dispatch.h"

static void deref_audio_sample(AudioSample &Sample)
{
   release_audio_callback(Sample.Callback);
   release_audio_callback(Sample.OnStop);
}

#if !defined(ALSA_ENABLED) and !defined(_WIN32)
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
   #ifdef _WIN32
   const double elapsed = QueuedAt ? double(PreciseTime() - QueuedAt) / 1000000.0 : 0;
   return std::max(0.0, audio_latency(QueuedFrames, OutputRate) - elapsed);
   #elif defined(ALSA_ENABLED)
   return audio_latency(BufferFrames, OutputRate);
   #else
   return 0;
   #endif
}

inline void extAudio::finish(AudioChannel &Channel, bool Notify) {
   Channel.Paused = false;
   if (!Channel.isStopped()) {
      Channel.State = CHS::FINISHED;
      if ((Channel.SampleHandle) and (Notify)) {
         #ifdef AUDIO_WORKER
            if (NotificationCount < Notifications.size()) {
               Notifications[NotificationCount++] = { Channel.SampleHandle, Channel.Handle,
                  Samples[Channel.SampleHandle].Generation, Channel.PlaybackGeneration,
                  PreciseTime() + int64_t(MixerLag() * 1000000) };
            }
            else {
               // Preserve completions during an extended client stall without allocating on the worker.
               auto &sample = Samples[Channel.SampleHandle];
               ++sample.DeferredStops;
               sample.DeferredStopDue = PreciseTime() + int64_t(MixerLag() * 1000000);
            }
            notify_audio(this);
         #else
            audio_stopped_event(*this, Channel.SampleHandle);
         #endif
      }
   }
   else Channel.State = CHS::FINISHED;
}

/*********************************************************************************************************************
-ACTION-
Activate: Enables access to the audio hardware and initialises the mixer.

An audio object must be activated before it can play audio samples. The activation process involves several critical steps:
hardware resource acquisition, mixer buffer allocation, and platform-specific driver initialisation.

Activation attempts to gain exclusive or shared access to the audio hardware device. On some platforms, this may fail if
another process has obtained an exclusive lock on the audio device. The specific behaviour depends on the underlying
audio system (ALSA on Linux, WASAPI on Windows).

If activation fails, the audio object remains in an inactive state but retains its configuration. Common failure
causes include hardware device unavailability, insufficient system resources, or driver compatibility issues.

All resources and device locks obtained during activation can be released through #Deactivate(). An inactive audio
object can perform configuration operations but cannot process audio samples.

-ERRORS-
Okay: Hardware activation completed successfully.
CreateResource: The hardware audio buffer could not be created.
Activate: The hardware device could not begin playback.

*********************************************************************************************************************/

static ERR AUDIO_Activate(extAudio *Self)
{
   kt::Log log;

#ifdef AUDIO_WORKER
   if (Self->WorkerStarted and !Self->StopWorker) return ERR::Okay;

   if (Self->WorkerStarted) {
      #ifdef ALSA_ENABLED
      free_alsa(Self);
      #else
      stop_audio_worker(Self);
      #endif
   }
#endif

   if (Self->Initialising) return ERR::Okay;

   log.branch();

   Self->Initialising = true;

   ERR error;
   if ((error = init_audio(Self)) != ERR::Okay) {
      Self->Initialising = false;
#ifdef AUDIO_WORKER
      stop_audio_worker(Self);
#endif
#ifdef ALSA_ENABLED
      free_alsa(Self);
#endif
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

#ifdef _WIN32
   const auto mix_buffer_size = BYTELEN(Self->BufferFrames * mixbitsize);
#else
   const auto mix_buffer_size = BYTELEN((int((mixbitsize * Self->OutputRate) * (MIX_INTERVAL * 1.5)) + 15) & (~15));
#endif

   Self->MixBuffer.resize(mix_buffer_size / sizeof(float));
   Self->MixElements = SAMPLE(mix_buffer_size / mixbitsize);

   {
      // Activation may negotiate a new rate/layout or resize the mix buffer after deactivation.

      if (auto result = configure_effects(*Self->GlobalEffects, Self->OutputRate, Self->Stereo);
          result != ERR::Okay) {
         Self->Initialising = false;
         acDeactivate(Self);
         return result;
      }

      for (auto &set : Self->Sets) {
         if (!set.Effects) continue;
         set.ScratchBuffer.resize(Self->MixBuffer.size());
         if (auto result = configure_effects(*set.Effects, Self->OutputRate, Self->Stereo); result != ERR::Okay) {
            Self->Initialising = false;
            acDeactivate(Self);
            return result;
         }
      }
   }

   Self->EffectConfigured = true;

   // Configure the mixing system

   bool use_interpolation = (Self->Flags & ADF::OVER_SAMPLING) != ADF::NIL;
   Self->MixConfig = AudioConfig(Self->Stereo, use_interpolation);

   Self->Initialising = false;

#ifdef AUDIO_WORKER
   auto worker_error = start_audio_worker(Self);
   if (worker_error != ERR::Okay) stop_audio_worker(Self);
   #ifdef ALSA_ENABLED
      if (worker_error != ERR::Okay) free_alsa(Self);
   #endif
   return worker_error;
#else
   return ERR::Okay;
#endif
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
array(char) Data: Points to the address of the sample data.
struct(*AudioLoop) Loop: Optional sample loop information.
&int Result: The resulting sample handle will be returned in this parameter.

-ERRORS-
Okay: Sample successfully added to the audio system.
Args: Invalid argument values provided.
NullArgs: Required parameters are null or missing.
AllocMemory: Failed to allocate enough memory to hold the sample data.

-TAGS-
mutates-object, copies-input, callback-held, creates-resource
-END-

*********************************************************************************************************************/

ERR AUDIO_AddSample(extAudio *Self, struct snd::AddSample *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.branch("Data: %p, Length: %zu", Args->Data.data(), Args->Data.size_bytes());

   if (Args->Data.size_bytes() > size_t(INT_MAX)) return log.warning(ERR::Args);

   const int shift = sample_shift(Args->SampleFormat);
   const int64_t frame_bytes = int64_t(1) << shift;
   if ((Args->Data.size_bytes() % frame_bytes) != 0) return log.warning(ERR::Args);
   if (Args->Loop) {
      if (Args->Loop->Loop1Type != LTYPE::NIL and
          (Args->Loop->Loop1Start < 0 or Args->Loop->Loop1End < Args->Loop->Loop1Start or
          Args->Loop->Loop1End > int64_t(Args->Data.size_bytes()) or
          (Args->Loop->Loop1Start % frame_bytes) or (Args->Loop->Loop1End % frame_bytes))) {
         return log.warning(ERR::Args);
      }
      if (Args->Loop->Loop2Type != LTYPE::NIL and
          (Args->Loop->Loop2Start < 0 or Args->Loop->Loop2End < Args->Loop->Loop2Start or
          Args->Loop->Loop2End > int64_t(Args->Data.size_bytes()) or
          (Args->Loop->Loop2Start % frame_bytes) or (Args->Loop->Loop2End % frame_bytes))) {
         return log.warning(ERR::Args);
      }
   }

   std::vector<uint8_t> data;
   if (Args->SampleFormat != SFM::NIL) data.assign(Args->Data.begin(), Args->Data.end());
   std::vector<AudioSample> sample_slots;
   if (Self->Samples.size() + 10 > Self->Samples.capacity()) sample_slots.reserve(Self->Samples.size() + 10);
   std::lock_guard mixer_lock(Self->MixerMutex);

   // Find an unused sample block.  If there is none, increase the size of the sample management area.

   int idx;
   for (idx=1; idx < std::ssize(Self->Samples); idx++) {
      if (Self->Samples[idx].Data.empty()) break;
   }

   if (idx >= std::ssize(Self->Samples)) {
      if (sample_slots.capacity()) {
         for (auto &entry : Self->Samples) sample_slots.emplace_back(std::move(entry));
         Self->Samples.swap(sample_slots);
      }
      Self->Samples.resize(Self->Samples.size() + 10);
   }

   auto &sample = Self->Samples[idx];
   deref_audio_sample(sample);
   sample.clear();
   sample.SampleType   = Args->SampleFormat;
   sample.SampleLength = SAMPLE(Args->Data.size_bytes() >> shift);
   sample.BufferedLength = BYTELEN(0);
   sample.OnStop       = Args->OnStop;
   if (sample.OnStop.defined()) sample.OnStop.pin();

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

   sample.Data.swap(data);

   Args->Result = idx;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
AddStream: Adds a new sample-stream to an Audio object for channel-based playback.

Use AddStream to load large sound samples to an Audio object, allowing it to play those samples on the client
machine without over-provisioning available resources.  For small samples under 256k consider using #AddSample()
instead.

The data source used for a stream must be provided by a client callback.  The native prototype is
`INT callback(INT SampleHandle, INT64 Offset, UINT8 *Buffer, INT BufferSize)`.

The `Offset` is the 64-bit byte position of the decoded data.  The `Buffer` and `BufferSize` identify the destination.
The callback must return the number of bytes written, or zero when no data is currently available.  Offset, stream
length, play offset and loop boundaries must be aligned to a complete source frame.

On ALSA and Windows, callbacks run on the client thread and fill a frame-aligned source ring.  The saved
`StreamBufferMs` setting
requests 100–10000 ms of source data at #OutputRate (default 1000 ms), capped at 8 MiB per stream and 64 MiB per Audio
object.  Refills are requested at half capacity.  Initial playback uses the first fill; a short fill permits playback
with the available data.  A zero-byte result is retried without blocking other sounds.  If the ring empties, that source
contributes silence and resumes from its preserved position when data arrives.  Source prefetch adds no output queue
latency.  Sources played faster than #OutputRate exhaust the ring proportionally sooner.

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
func Callback: This callback function must be able to return raw audio data for streaming.  The function context will be pinned as a safety measure.
func OnStop: This optional callback function will be called when the stream stops playing.  The function context will be pinned as a safety measure.
int(SFM) SampleFormat: Indicates the format of the sample data that you are adding.
large SampleLength: Total byte length of the stream, or `-1` when the length is unknown.  Zero is also accepted as the legacy unknown-length value.
large PlayOffset: Initial byte position.  It must be aligned to a complete source frame.
struct(*AudioLoop) Loop: Refers to sample loop information, or `NULL` if no loop is required.
&int Result: The resulting sample handle will be returned in this parameter.

-ERRORS-
Okay: Stream successfully configured and added to the audio system.
Args: Invalid argument values provided.
OutOfRange: A length, play offset or loop boundary is outside the stream.
NullArgs: Required parameters are null or missing.
AllocMemory: Failed to allocate the stream buffer.

-TAGS-
mutates-object, retains-input, callback-held, creates-resource
-END-

*********************************************************************************************************************/

static const int MAX_STREAM_BUFFER = 16 * 1024; // Max stream buffer length in bytes

static ERR AUDIO_AddStream(extAudio *Self, struct snd::AddStream *Args)
{
   kt::Log log;

   if ((!Args) or (Args->SampleFormat IS SFM::NIL)) return log.warning(ERR::NullArgs);
   if (Args->Callback.Type IS CALL::NIL) return log.warning(ERR::NullArgs);
   if (Args->SampleLength < -1 or Args->PlayOffset < 0) return ERR::OutOfRange;
   if (Args->SampleLength > 0 and Args->PlayOffset > Args->SampleLength) return ERR::OutOfRange;
   const int shift = sample_shift(Args->SampleFormat);
   const int64_t frame_bytes = int64_t(1) << shift;
   if ((Args->PlayOffset % frame_bytes) or
       (Args->SampleLength > 0 and Args->SampleLength % frame_bytes)) return ERR::Args;
   if (Args->Loop and (Args->Loop->Loop1Start < 0 or Args->Loop->Loop1End < Args->Loop->Loop1Start or
       (Args->SampleLength > 0 and Args->Loop->Loop1End > Args->SampleLength))) return ERR::OutOfRange;
   if (Args->Loop and ((Args->Loop->Loop1Start % frame_bytes) or
       (Args->Loop->Loop1End % frame_bytes))) return ERR::Args;

   log.branch("Length: %" PF64, (long long)Args->SampleLength);

   int buffer_len;
   #ifdef AUDIO_WORKER
   // Cap each source at 8 MiB and all source rings together at 64 MiB.
   buffer_len = int(std::min<int64_t>((int64_t(Self->OutputRate) * Self->StreamBufferMs / 1000) << shift,
      8 * 1024 * 1024));
   buffer_len = std::max(2 << shift, buffer_len);
   #else
   buffer_len = Args->SampleLength > 0 ? int(std::min<int64_t>(Args->SampleLength / 2, MAX_STREAM_BUFFER)) :
      MAX_STREAM_BUFFER;
   #endif

   std::vector<uint8_t> data(buffer_len);
   std::vector<AudioSample> sample_slots;
   if (Self->Samples.size() + 10 > Self->Samples.capacity()) sample_slots.reserve(Self->Samples.size() + 10);

   std::lock_guard mixer_lock(Self->MixerMutex);

#ifdef AUDIO_WORKER
   size_t allocated = 0;
   for (const auto &entry : Self->Samples) if (entry.Stream) allocated += entry.Data.size();
   if (allocated + buffer_len > 64 * 1024 * 1024) return ERR::AllocMemory;
#endif

   // Find an unused sample block.  If there is none, increase the size of the sample management area.

   int idx;
   for (idx=1; idx < std::ssize(Self->Samples); idx++) {
      if (Self->Samples[idx].Data.empty()) break;
   }

   if (idx >= std::ssize(Self->Samples)) {
      if (sample_slots.capacity()) {
         for (auto &entry : Self->Samples) sample_slots.emplace_back(std::move(entry));
         Self->Samples.swap(sample_slots);
      }
      Self->Samples.resize(Self->Samples.size() + 10);
   }

   // Setup the audio sample

   auto &sample = Self->Samples[idx];
   deref_audio_sample(sample);
   sample.clear();
   sample.SampleType   = Args->SampleFormat;
   sample.SampleLength = SAMPLE(buffer_len>>shift);
   sample.StreamLength = BYTELEN(std::max<int64_t>(0, Args->SampleLength));
   sample.StreamLengthKnown = Args->SampleLength > 0;
   sample.Callback     = Args->Callback;
   sample.OnStop       = Args->OnStop;
   sample.BufferedLength = BYTELEN(0);
   if (sample.Callback.defined()) sample.Callback.pin();
   if (sample.OnStop.defined()) sample.OnStop.pin();
   sample.Stream       = true;
   sample.PlayPos      = BYTELEN(Args->PlayOffset);

   sample.LoopMode     = LOOP::SINGLE;
   sample.Loop1Type    = LTYPE::UNIDIRECTIONAL;
   sample.Loop1Start   = SAMPLE(0);
   sample.Loop1End     = SAMPLE(buffer_len>>shift);

   if (Args->Loop) {
      sample.Loop2Type    = LTYPE::UNIDIRECTIONAL;
      sample.Loop2Start   = SAMPLE(Args->Loop->Loop1Start >> shift);
      sample.Loop2End     = SAMPLE(Args->Loop->Loop1End >> shift);
      sample.StreamLength = BYTELEN(sample.Loop2End<<shift);
      sample.StreamLengthKnown = true;

      if (sample.Loop2Start IS sample.Loop2End) sample.Loop2Type = LTYPE::NIL;
   }

   sample.Data.swap(data);
   #ifdef AUDIO_WORKER
   ++sample.Generation;
   sample.Ring.Read = sample.Ring.Used = 0;
   #endif
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

-TAGS-
blocking

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
   if (wasapi_beep(Args->Pitch, Args->Duration)) {
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

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR AUDIO_CloseChannels(extAudio *Self, struct snd::CloseChannels *Args)
{
   std::lock_guard mixer_lock(Self->MixerMutex);
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.branch("Handle: $%.8x", Args->Handle);

   int index = Args->Handle>>16;
   if ((index < 1) or (index >= std::ssize(Self->Sets))) return log.warning(ERR::Args);

   cancel_audio_batches(Self, unsigned(index));
   ++*Self->GlobalEffects->Generation;
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
   kt::Log log;

   log.branch();

   if (Self->Initialising) {
      log.msg("Audio is still in the process of initialisation.");
      return ERR::Okay;
   }

   acClear(Self);

#ifdef AUDIO_WORKER
   stop_audio_worker(Self);
#else
   std::lock_guard mixer_lock(Self->MixerMutex);
   cancel_audio_batches(Self);
#endif

   {
      std::lock_guard lock(Self->MixerMutex);
      Self->EffectConfigured = false;
      Self->GlobalEffects->reset();
      Self->GlobalEffects->Rate = 0;
      for (auto &set : Self->Sets) if (set.Effects) {
         set.Effects->reset();
         set.Effects->Rate = 0;
      }
   }

#ifdef ALSA_ENABLED
   free_alsa(Self);
#endif

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR AUDIO_Init(extAudio *Self)
{
   kt::Log log;


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

-TAGS-
mutates-object, creates-resource
-END-

*********************************************************************************************************************/

static ERR AUDIO_OpenChannels(extAudio *Self, struct snd::OpenChannels *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.branch("Total: %d", Args->Total);

   Args->Result = 0;
   if ((Args->Total < 0) or (Args->Total > 64)) {
      return log.warning(ERR::OutOfRange);
   }

   ChannelSet channels;
   channels.Channel.resize(Args->Total);
   #ifdef AUDIO_WORKER
   channels.Shadow.resize(Args->Total); // Quality may enable oversampling during playback.
   #else
   if ((Self->Flags & ADF::OVER_SAMPLING) != ADF::NIL) channels.Shadow.resize(Args->Total);
   #endif
   channels.Tempo = 125;
   channels.MixLeft = channels.TickFrames(Self->OutputRate);
   channels.Commands.reserve(1024);

   std::vector<ChannelSet> set_slots;
   if (Self->Sets.size() + 2 > Self->Sets.capacity()) set_slots.reserve(Self->Sets.size() + 2);
   std::lock_guard mixer_lock(Self->MixerMutex);
   const int index = Self->Sets.size() + 1; // Set zero remains a dummy entry.
   if (set_slots.capacity()) {
      for (auto &entry : Self->Sets) set_slots.emplace_back(std::move(entry));
      Self->Sets.swap(set_slots);
   }
   Self->Sets.resize(index + 1);
   Self->Sets[index] = std::move(channels);

   for (int channel = 0; channel < Args->Total; ++channel) {
      const int handle = (index << 16) | channel;
      Self->Sets[index].Channel[channel].Handle = handle;
      if (channel < std::ssize(Self->Sets[index].Shadow)) Self->Sets[index].Shadow[channel].Handle = handle;
   }

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

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR AUDIO_RemoveSample(extAudio *Self, struct snd::RemoveSample *Args)
{
   std::vector<uint8_t> retired_data;
   std::lock_guard mixer_lock(Self->MixerMutex);
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.branch("Sample: %d", Args->Handle);

   if ((Args->Handle < 1) or (Args->Handle >= std::ssize(Self->Samples))) return log.warning(ERR::OutOfRange);

   #ifdef AUDIO_WORKER
   flush_audio_commands(Self);
   #endif
   for (auto &set : Self->Sets) {
      for (auto &command : set.Commands) {
         if (command.CommandID IS CMD::SAMPLE and std::get<int>(command.Data) IS Args->Handle) {
            command.DeferredError = ERR::NoData;
         }
      }
      for (auto &channel : set.Channel) {
         if (channel.SampleHandle IS Args->Handle) {
            channel.State = CHS::STOPPED;
            channel.Paused = false;
            channel.SampleHandle = 0;
         }
      }
      for (auto &channel : set.Shadow) {
         if (channel.SampleHandle IS Args->Handle) {
            channel.State = CHS::STOPPED;
            channel.SampleHandle = 0;
         }
      }
   }
   deref_audio_sample(Self->Samples[Args->Handle]);
   Self->Samples[Args->Handle].Data.swap(retired_data);
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
   kt::Log log;

   if ((!Args) or (!Args->Dest)) return log.warning(ERR::NullArgs);

   objConfig::create config = { };
   if (config.ok()) {
      config->write("AUDIO", "OutputRate", std::to_string(Self->OutputRate));
      config->write("AUDIO", "InputRate", std::to_string(Self->InputRate));
      config->write("AUDIO", "Quality", std::to_string(Self->Quality));
      config->write("AUDIO", "BitDepth", std::to_string(Self->BitDepth));
      config->write("AUDIO", "Periods", std::to_string(Self->Periods));
      config->write("AUDIO", "StreamBufferMs", std::to_string(Self->StreamBufferMs));
      config->write("AUDIO", "PeriodFrames", std::to_string(Self->PeriodSize));

      if ((Self->Flags & ADF::STEREO) != ADF::NIL) config->write("AUDIO", "Stereo", "TRUE");
      else config->write("AUDIO", "Stereo", "FALSE");

#ifdef __linux__
      if (!Self->Device.empty()) config->write("AUDIO", "Device", Self->Device);
      else config->write("AUDIO", "Device", "default");

      if ((!Self->Volumes.empty()) and ((Self->Flags & ADF::SYSTEM_WIDE) != ADF::NIL)) {
#ifdef ALSA_ENABLED
         snd_mixer_selem_id_t *sid;
         snd_mixer_selem_id_alloca(&sid);
         snd_mixer_selem_id_set_index(sid, 0);
#endif

         for (int i=0; i < std::ssize(Self->Volumes); i++) {
            auto channels = Self->Volumes[i].Channels;
            auto flags = Self->Volumes[i].Flags;

#ifdef ALSA_ENABLED
            if (Self->MixHandle) {
               snd_mixer_selem_id_set_name(sid, Self->Volumes[i].Name.c_str());
               if (auto elem = snd_mixer_find_selem(Self->MixHandle, sid)) {
                  long left = 0;
                  long right = 0;
                  long pmin = 0;
                  long pmax = 0;
                  int unmuted = 1;

                  if (snd_mixer_selem_has_playback_volume(elem)) {
                     snd_mixer_selem_get_playback_volume_range(elem, &pmin, &pmax);
                     snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &left);
                     snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &unmuted);
                     if ((flags & VCF::MONO) != VCF::NIL) right = left;
                     else snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_RIGHT, &right);
                  }
                  else if (snd_mixer_selem_has_capture_volume(elem)) {
                     snd_mixer_selem_get_capture_volume_range(elem, &pmin, &pmax);
                     snd_mixer_selem_get_capture_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &left);
                     snd_mixer_selem_get_capture_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &unmuted);
                     if ((flags & VCF::MONO) != VCF::NIL) right = left;
                     else snd_mixer_selem_get_capture_volume(elem, SND_MIXER_SCHN_FRONT_RIGHT, &right);
                  }

                  if (pmin < pmax) {
                     channels.resize(((flags & VCF::MONO) != VCF::NIL) ? 1 : 2);
                     channels[0] = (float)((double)(left - pmin) / (double)(pmax - pmin));
                     if ((flags & VCF::MONO) IS VCF::NIL) {
                        channels[1] = (float)((double)(right - pmin) / (double)(pmax - pmin));
                     }
                     if (unmuted) flags &= ~VCF::MUTE;
                     else flags |= VCF::MUTE;
                  }
               }
            }
#endif

            std::ostringstream out;
            if ((flags & VCF::MUTE) != VCF::NIL) out << "1,[";
            else out << "0,[";

            if ((flags & VCF::MONO) != VCF::NIL) {
               out << channels[0];
            }
            else for (int c=0; c < std::ssize(channels); c++) {
               if (c > 0) out << ',';
               out << channels[c];
            }
            out << ']';

            config->write("MIXER", Self->Volumes[i].Name.c_str(), out.str());
         }
      }

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
OutOfRange
NoSupport: Sample is not a stream.

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR AUDIO_SetSampleLength(extAudio *Self, struct snd::SetSampleLength *Args)
{
   std::lock_guard mixer_lock(Self->MixerMutex);
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.msg("Sample: #%d, Length: %" PF64, Args->Sample, (long long)Args->Length);

   if ((Args->Sample < 0) or (Args->Sample >= std::ssize(Self->Samples))) return log.warning(ERR::Args);

   auto &sample = Self->Samples[Args->Sample];

   if (sample.Stream) {
      if (Args->Length < -1) return ERR::OutOfRange;
      const int64_t frame_bytes = int64_t(1) << sample_shift(sample.SampleType);
      if (Args->Length >= 0 and Args->Length % frame_bytes) return ERR::Args;
      if (Args->Length >= 0 and sample.Loop2Type != LTYPE::NIL and
          Args->Length <= (int64_t(sample.Loop2Start) << sample_shift(sample.SampleType))) return ERR::OutOfRange;
      sample.StreamLength = BYTELEN(std::max<int64_t>(0, Args->Length));
      sample.StreamLengthKnown = Args->Length >= 0;
      #ifdef AUDIO_WORKER
      sample.EndOfSource = sample.StreamLengthKnown and sample.SourceOffset >= sample.StreamLength;
      if (sample.StreamLengthKnown) {
         const int64_t remaining = std::max<int64_t>(0, sample.StreamLength - sample.PlayPos);
         sample.Ring.Used = std::min(sample.Ring.Used, size_t(remaining));
      }
      sample.Ring.Used -= sample.Ring.Used % (1 << sample_shift(sample.SampleType));
      #endif
      return ERR::Okay;
   }
   else return log.warning(ERR::NoSupport);
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
strview Name: If the correct index number is unknown, the name of the mixer may be set here.
int(SVF) Flags: Optional flags.
int Channel: A specific channel to modify (e.g. `0` for left, `1` for right).  If `-1`, all channels are affected.
double Volume: The volume to set for the mixer, from 0 to 1.0.  If `-1`, the current volume values are retained.

-ERRORS-
Okay: The new volume was applied successfully.
Args
NullArgs
OutOfRange: The `Volume` or `Index` is out of the acceptable range.
NoSupport
NotInitialised
Search

-TAGS-
blocking, mutates-object
-END-

*********************************************************************************************************************/

static ERR AUDIO_SetVolume(extAudio *Self, struct snd::SetVolume *Args)
{
   std::unique_lock mixer_lock(Self->MixerMutex);
   kt::Log log;

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
   if (!Self->MixHandle) {
      for (auto &volume : Self->Volumes) {
         if (!iequals("Master", volume.Name)) continue;
         if (Args->Volume >= 0) {
            Self->MasterVolume = Args->Volume;
            volume.Channels[0] = Args->Volume;
         }
         if ((Args->Flags & SVF::MUTE) != SVF::NIL) {
            Self->Mute = true;
            volume.Flags |= VCF::MUTE;
         }
         if ((Args->Flags & SVF::UNMUTE) != SVF::NIL) {
            Self->Mute = false;
            volume.Flags &= ~VCF::MUTE;
         }
         break;
      }
      return ERR::Okay;
   }

   // Determine what mixer we are going to adjust

   if (not Args->Name.empty()) {
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

   mixer_lock.unlock();

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

   if (not Args->Name.empty()) {
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

Windows uses a 32-bit float mixer and converts to the endpoint format at the output boundary.
ALSA supports `8`, `16` and `32` (floating point) output.  The recommended value for CD quality playback is `16`.

*********************************************************************************************************************/

static ERR SET_BitDepth(extAudio *Self, int Value)
{
   if (Value IS 16) Self->BitDepth = 16;
   else if (Value IS 8) Self->BitDepth = 8;
   else if (Value IS 24) Self->BitDepth = 24;
   else if (Value IS 32) Self->BitDepth = 32;
   else return ERR::InvalidValue;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Device: The name of the audio device used by this audio object.

A host platform may have multiple audio devices installed, but a given audio object can represent only one device
at a time.  A new audio object will always represent the default device initially.  Choose a different device by
setting the `Device` field to a valid alternative.

The default device can always be referenced with a name of `default`.  Windows also accepts a WASAPI endpoint ID.
ALSA PCM names such as `hw:0,0` and `plughw:0,0` are also accepted.  Direct hardware access can avoid sound-server
buffering, but may require exclusive access and stricter sample-format support.

*********************************************************************************************************************/

static ERR GET_Device(extAudio *Self, std::string_view &Value)
{
   Value = Self->Device;
   if (not Self->Device.empty()) return ERR::Okay;
   else return ERR::FieldNotSet;
}

static ERR SET_Device(extAudio *Self, const std::string_view &Value)
{
   if (Value.empty()) Self->Device = "default";
   else {
      Self->Device.assign(Value);
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

On Windows this field estimates the outstanding submitted frames from the most recent endpoint padding and render
time.  It can be zero while idle, so an idle reading is not a buffer-capacity or future callback-latency estimate.
On ALSA it is the negotiated buffer size in frames divided by the output rate, and is zero while inactive.
It excludes client scheduling, source starvation, and downstream sound-server or wireless-device buffering; it is not
an acoustic latency guarantee.  Source prefetch uses the saved `StreamBufferMs` setting (100–10000ms, default
1000ms) independently of this value.

*********************************************************************************************************************/

static ERR GET_MixerLag(extAudio *Self, double *Value)
{
   std::lock_guard mixer_lock(Self->MixerMutex);
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
   else if (Value > 192000) Self->OutputRate = 192000;
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
<li><b>Recommended</b>: 3 periods (balances latency and reliability for most use cases)</li>
</list>

Fewer periods reduce overall system latency but increase the risk of audio dropouts if processing cannot keep pace with
audio consumption. More periods provide greater buffering security at the cost of increased latency.

*********************************************************************************************************************/

static ERR SET_Periods(extAudio *Self, int Value)
{
   Self->Periods = audio_period_count(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
PeriodSize: Defines the number of frames in each ALSA period.

A frame contains one sample for every output channel.  Period duration is `PeriodSize / OutputRate` seconds,
independent of bit depth and channel count.  The default ALSA request is 256 frames in three periods.
Values are clamped to 32–16384 frames before activation; ALSA may negotiate a different value.
After activation this field reports the negotiated period size.

*********************************************************************************************************************/

static ERR SET_PeriodSize(extAudio *Self, int Value)
{
   Self->PeriodSize = audio_period_frames(Value);
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
   std::lock_guard mixer_lock(Self->MixerMutex);
   Self->Quality = Value;

   Self->Flags &= ~(ADF::FILTER_LOW|ADF::FILTER_HIGH|ADF::OVER_SAMPLING);

   if (Self->Quality < 10) {}
   else if (Self->Quality < 33) Self->Flags |= ADF::FILTER_LOW;
   else if (Self->Quality < 66) Self->Flags |= ADF::FILTER_HIGH;
   else Self->Flags |= ADF::OVER_SAMPLING|ADF::FILTER_HIGH;
   Self->MixConfig = AudioConfig(Self->Stereo, (Self->Flags & ADF::OVER_SAMPLING) != ADF::NIL);

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
   std::lock_guard mixer_lock(Self->MixerMutex);
   if (Value IS TRUE) Self->Flags |= ADF::STEREO;
   else Self->Flags &= ~ADF::STEREO;
   return ERR::Okay;
}

//********************************************************************************************************************

extAudio::extAudio(objMetaClass *ClassPtr, OBJECTID ObjectID) : objAudio(ClassPtr, ObjectID)
{
   OutputRate  = 44100;        // Rate for output to speakers
   InputRate   = 44100;        // Input rate for recording
   Quality     = 80;
   BitDepth    = 16;
   Flags       = ADF::OVER_SAMPLING|ADF::FILTER_HIGH|ADF::VOL_RAMPING|ADF::STEREO;
   #ifdef ALSA_ENABLED
   Periods     = 3;
   PeriodSize  = 256;
   #else
   Periods     = 4;
   PeriodSize  = 2048;
   #endif
   MaxChannels = 8;
   Device      = glAudioDevice.empty() ? "default" : glAudioDevice;
   MasterVolume = 1.0;

   const SystemState *state = GetSystemState();
   if ((iequals(state->Platform, "Native")) or (iequals(state->Platform, "Linux"))) {
      Flags |= ADF::SYSTEM_WIDE;
   }

   Samples.reserve(32);

#ifdef __linux__
   Volumes.resize(2);
   Volumes[0].Name = "Master";

   Volumes[1].Name = "PCM";
#else
   Volumes.resize(1);
   Volumes[0].Name = "Master";
   Volumes[0].Channels[0] = 1.0;
   for (int i=1; i < std::ssize(Volumes[0].Channels); i++) Volumes[0].Channels[i] = -1;
#endif

   kt::Log log("New");

   // Attempt to get the user's preferred audio settings from user:config/audio.cfg.

   objConfig::create config = { fl::Path("user:config/audio.cfg") };

   if (config.ok()) {
      config->read("AUDIO", "OutputRate", OutputRate);
      config->read("AUDIO", "InputRate", InputRate);
      config->read("AUDIO", "Quality", Quality);
      config->read("AUDIO", "BitDepth", BitDepth);

      int value;
      if (!config->read("AUDIO", "StreamBufferMs", value)) StreamBufferMs = std::clamp(value, 100, 10000);
      if (!config->read("AUDIO", "Periods", value)) SET_Periods(this, value);
      if (glAudioDevice.empty()) config->read("AUDIO", "Device", Device);

      std::string str;
      Flags |= ADF::STEREO;
      if (!config->read("AUDIO", "Stereo", str)) {
         if (iequals("FALSE", str)) Flags &= ~ADF::STEREO;
      }

      if ((BitDepth != 8) and (BitDepth != 16) and (BitDepth != 24) and (BitDepth != 32)) BitDepth = 16;
      if (!config->read("AUDIO", "PeriodFrames", value)) SET_PeriodSize(this, value);
      SET_Quality(this, Quality);

      // Find the mixer section, then load the mixer information

      ConfigGroups *groups;
      if (!config->getGroups(groups)) {
         for (auto & [group, keys] : groups[0]) {
            if (iequals("MIXER", group)) {
               Volumes.clear();
               Volumes.resize(keys.size());

               int j = 0;
               for (auto & [k, v] : keys) {
                  Volumes[j].Name = k;

                  CSTRING str = v.c_str();
                  if (std::stoi(v) IS 1) Volumes[j].Flags |= VCF::MUTE;
                  while ((*str) and (*str != ',')) str++;
                  if (*str IS ',') str++;

                  int channel = 0;
                  if (*str IS '[') { // Read channel volumes
                     str++;
                     while ((*str) and (*str != ']')) {
                        Volumes[j].Channels[channel] = strtol(str, nullptr, 0);
                        while ((*str) and (*str != ',') and (*str != ']')) str++;
                        if (*str IS ',') str++;
                        channel++;
                     }
                  }

                  while (channel < (int)Volumes[j].Channels.size()) {
                     Volumes[j].Channels[channel] = 0.75;
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

extAudio::~extAudio() {
   if ((Flags & ADF::AUTO_SAVE) != ADF::NIL) saveSettings();

   if (Timer) { UpdateTimer(Timer, 0); Timer = nullptr; }

   #ifdef AUDIO_WORKER
   stop_audio_worker(this);
   #endif
   if (BatchTimer) { UpdateTimer(BatchTimer, 0); BatchTimer = nullptr; }
   BatchCompletions.clear([](FUNCTION &Callback) { release_audio_callback(Callback); });
   for (auto &sample : Samples) deref_audio_sample(sample);

   glSoundChannels.erase(UID);

   acDeactivate(this);

#ifdef ALSA_ENABLED

   free_alsa(this);


#endif
}

/*********************************************************************************************************************
-METHOD-
GetEffectStatus: Reads DSP drain state and serial-path latency together.

GetEffectStatus() returns a consistent snapshot of one signal path's effect latency and drain state, taken under the
mixer lock so that all output values describe the same moment.

The path is selected with `Channel`.  A channel-set handle selects that application's chain followed by the global
chain.  Zero selects the global path, in which case `Application` is always zero and the drain state accounts for
every application's sources and chains.  Latencies are expressed in frames at the returned `Rate`.  `Total` is the
sum of `Application` and `Global`, and only includes effects that are not bypassed.

`State` takes one of the following values:

!ADS

`Truncated` is set to 1 if the selected chain's automatic drain was cut short at the #MaxDrain deadline while effects
still had pending output.  It is cleared when new source audio reaches the chain or the chain is reset.

`Generation` is incremented whenever the effect configuration or DSP history of any path changes, and is written
even when the call fails.  Compare it between calls to detect intervening resets or reconfiguration.  The remaining
outputs are not meaningful if an error is returned.

-INPUT-
int Channel:        Channel-set handle, or zero for the global path.
&large Application: Application-chain algorithmic latency.
&large Global:      Global-chain algorithmic latency.
&large Total:       Total algorithmic latency along this path.
&int Rate:          Negotiated output rate, or zero before configuration.
&large Generation:  Configuration generation shared by all paths.
&int(ADS) State:    Selected chain's drain state.
&int Truncated:     One if automatic drain reached its safety deadline.

-ERRORS-
Okay
NullArgs
Args: The channel-set handle is invalid or closed.
NotInitialised: No output configuration is available; Rate is zero.
OutOfRange: Serial latency cannot be represented in a signed 64-bit frame count.

-END-
*********************************************************************************************************************/

static AudioEffectChain *effect_chain(extAudio *Self, int Channel)
{
   if (!Channel) return Self->GlobalEffects.get();
   const auto index = unsigned(Channel) >> 16;
   if (Channel < 0 or (Channel & 0xffff) or index >= Self->Sets.size() or
       Self->Sets[index].Channel.empty()) return nullptr;
   return Self->Sets[index].Effects.get();
}

static bool effect_channel_valid(extAudio *Self, int Channel)
{
   if (!Channel) return true;
   const auto index = unsigned(Channel) >> 16;
   return Channel > 0 and !(Channel & 0xffff) and index < Self->Sets.size() and
      !Self->Sets[index].Channel.empty();
}

static ERR AUDIO_GetEffectStatus(extAudio *Self, struct snd::GetEffectStatus *Args)
{
   if (!Args) return ERR::NullArgs;

   std::lock_guard lock(Self->MixerMutex);
   Args->Application = Args->Global = Args->Total = 0;
   Args->Rate = 0;
   Args->Generation = *Self->GlobalEffects->Generation;
   Args->State = ADS::IDLE;
   Args->Truncated = 0;

   if (!effect_channel_valid(Self, Args->Channel)) return ERR::Args;
   if (!Self->EffectConfigured) return ERR::NotInitialised;

   Args->Rate = Self->OutputRate;
   bool source_active = false, upstream_pending = false;
   for (size_t index = 1; index < Self->Sets.size(); ++index) {
      if (Args->Channel and index != (unsigned(Args->Channel) >> 16)) continue;
      auto &set = Self->Sets[index];
      for (auto &channel : set.Channel) source_active |= channel.active() and !channel.isStopped();
      for (auto &channel : set.Shadow) source_active |= channel.active() and !channel.isStopped();
      if (!Args->Channel and set.Effects) upstream_pending |= set.Effects->pending();
   }

   auto chain = effect_chain(Self, Args->Channel);
   bool path_pending = upstream_pending or (chain and chain->pending());
   if (Args->Channel) path_pending |= Self->GlobalEffects->pending();
   Args->State = source_active ? ADS::ACTIVE :
      (path_pending ? ADS::DRAINING : ADS::IDLE);

   Args->Truncated = (chain and chain->Truncated) or (Args->Channel and Self->GlobalEffects->Truncated);
   if (chain) {
      if (Args->Channel) {
         auto error = chain->latency(Args->Application);
         if (error != ERR::Okay) return error;
      }
   }

   if (auto error = Self->GlobalEffects->latency(Args->Global); error != ERR::Okay) return error;
   if (Args->Application > INT64_MAX - Args->Global) return ERR::OutOfRange;
   Args->Total = Args->Application + Args->Global;
   return ERR::Okay;
}

/*********************************************************************************************************************
-METHOD-
ResetEffects: Discards one chain's DSP history at its next render boundary.

ResetEffects() clears the internal state of every effect in one chain, such as filter history and buffered samples, so
that subsequent audio is processed as if the chain had just been created.  Use it when switching to unrelated content,
e.g. after seeking, so that residual output from the previous material does not bleed through.

The chain is selected with `Channel`.  A channel-set handle selects that application's chain, while zero selects the
global chain.  Only the selected chain is affected; resetting an application's chain does not reset the global chain.
If the channel set has no effects attached, the call succeeds without doing anything.

The reset is deferred to the mixer and each effect's history is cleared before it processes its next block of audio.
Any pending effect tail is discarded rather than drained.  Effect parameters and bypass states are unchanged.  The
chain's drain state returns to `ADS::IDLE`, its `Truncated` status is cleared, effect meters are restarted, and the
`Generation` counter reported by #GetEffectStatus() is incremented.

-INPUT-
int Channel: Channel-set handle, or zero for the global chain.

-ERRORS-
Okay
NullArgs
Args: The channel-set handle is invalid or closed.
-END-
*********************************************************************************************************************/

static ERR AUDIO_ResetEffects(extAudio *Self, struct snd::ResetEffects *Args)
{
   if (!Args) return ERR::NullArgs;

   std::lock_guard lock(Self->MixerMutex);
   if (!effect_channel_valid(Self, Args->Channel)) return ERR::Args;
   if (auto chain = effect_chain(Self, Args->Channel)) {
      chain->reset(); // Mark processors for reset; process() performs the bounded reset before the next sample.
   }
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
MaxDrain: Limits the time that DSP effect chains can continue to produce output after their source audio ends.

When the sources feeding an effect chain stop, effects such as reverbs, delays and recursive filters can continue to
produce output as their internal history decays.  This drain phase continues until the chain has no pending output,
or until MaxDrain seconds of drain time have elapsed.  The limit applies independently to each application chain and
to the global chain, and it resets when new source audio reaches a chain.

If a chain reaches the deadline while effects still have pending output, the remaining output is faded out over
approximately 10 ms and discarded, and the `Truncated` result of #GetEffectStatus() is set.  The limit acts as a
safety net for effects that have no finite tail, such as an @AudioEqualiser with long decays or feedback that never
fully settles.

The default is 30 seconds.  Valid values range from 0.01 to 300 seconds; `ERR::OutOfRange` is returned otherwise.
The value can only be changed while effects are not configured, i.e. before the Audio object is activated or
after it has been deactivated.  Otherwise, `ERR::InvalidState` is returned.

-END-
*********************************************************************************************************************/

static ERR GET_MaxDrain(extAudio *Self, double *Value)
{
   std::lock_guard lock(Self->MixerMutex);
   *Value = Self->MaxDrain;
   return ERR::Okay;
}

static ERR SET_MaxDrain(extAudio *Self, double Value)
{
   std::lock_guard lock(Self->MixerMutex);
   if (Self->EffectConfigured) return ERR::InvalidState;
   if (!std::isfinite(Value) or Value < 0.01 or Value > 300) return ERR::OutOfRange;
   Self->MaxDrain = Value;
   return ERR::Okay;
}

#include "class_audio_def.c"

static const FieldArray clAudioFields[] = {
   { "OutputRate",    FDF_INT|FDF_RI, nullptr, SET_OutputRate },
   { "InputRate",     FDF_INT|FDF_RI },
   { "Quality",       FDF_INT|FDF_RW,      nullptr, SET_Quality },
   { "Flags",         FDF_INTFLAGS|FDF_RI, nullptr, nullptr, &clAudioFlags },
   { "BitDepth",      FDF_INT|FDF_RI,      nullptr, SET_BitDepth },
   { "Periods",       FDF_INT|FDF_RI,      nullptr, SET_Periods },
   { "PeriodSize",    FDF_INT|FDF_RI,      nullptr, SET_PeriodSize },
   // VIRTUAL FIELDS
   { "MaxDrain",      FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, GET_MaxDrain, SET_MaxDrain },
   { "Device",        FDF_CPPSTRING|FDF_RW|FDF_PURE,  GET_Device, SET_Device },
   { "MixerLag",      FDF_DOUBLE|FDF_R,               GET_MixerLag },
   { "MasterVolume",  FDF_DOUBLE|FDF_RW|FDF_PURE,     GET_MasterVolume, SET_MasterVolume },
   { "Mute",          FDF_INT|FDF_RW,                 GET_Mute, SET_Mute },
   { "Stereo",        FDF_INT|FDF_RW|FDF_PURE,        GET_Stereo, SET_Stereo },
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
