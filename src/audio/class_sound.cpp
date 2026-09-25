/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Sound: High-level audio sample playback interface with intelligent resource management and format detection.

The Sound class provides a comprehensive, user-friendly interface for audio sample playback that abstracts the complexities
of audio hardware management whilst delivering professional-quality results.  Designed as the primary interface for
general-purpose audio operations, the Sound class automatically handles resource allocation, format detection, hardware
abstraction, and intelligent streaming decisions to provide optimal performance across diverse system configurations.

The Sound class implements robust file format support with automatic detection and validation:

<list type="bullet">
<li><b>Native WAVE Support:</b> Complete support for WAVE format files including all standard PCM encodings, multiple bit depths (8/16-bit), and both mono and stereo configurations</li>
<li><b>Automatic Format Detection:</b> File format identification occurs automatically during initialisation based on file headers and content analysis</li>
<li><b>Extensible Architecture:</b> Additional audio formats (MP3, OGG, FLAC, AAC) can be supported through Sound class extensions and codec plugins</li>
<li><b>Validation and Error Handling:</b> Comprehensive file validation prevents playback of corrupted or unsupported audio data</li>
</list>

The following demonstrates advanced Sound class usage including pitch control and event handling:

<pre>
snd = obj.new('sound', {
   path = 'audio:samples/piano_c4.wav',
   note = 'C6',    -- Play two octaves higher
   volume = 0.8,   -- Reduce volume to 80%
   onStop = function(Sound)
      print('Playback completed')
      processing.signal()
   end
})

snd.acActivate()
processing.sleep()  -- Wait for completion
</pre>

-END-

**********************************************************************************************************************

All playback uses the shared mixer, so application and global effects also apply to ordinary playback.

*********************************************************************************************************************/

#include <array>

constexpr int SIZE_RIFF_CHUNK = 12;

static ERR SOUND_GET_Active(extSound *, int *);
static ERR SOUND_GET_Elapsed(extSound *, double *);
static ERR SOUND_GET_PlayPosition(extSound *, int64_t *);
static ERR SOUND_GET_Progress(extSound *, double *);
static ERR SOUND_GET_Remaining(extSound *, double *);

static ERR SOUND_SET_Note(extSound *, const std::string_view &);

static const std::array<double, 12> glScale = {
   1.0,         // C
   1.059435080, // CS
   1.122424798, // D
   1.189198486, // DS
   1.259909032, // E
   1.334823988, // F
   1.414172687, // FS
   1.498299125, // G
   1.587356190, // GS
   1.681764324, // A
   1.781752857, // AS
   1.887704009  // B
};

static OBJECTPTR clSound = nullptr;

static ERR find_chunk(objFile *, std::string_view);

//********************************************************************************************************************
// Send a callback to the client when playback stops.

static void sound_stopped_event(extSound *Self)
{
   Self->Active = false;

   if (Self->OnStop.stale()) {
      Self->OnStop.unpin();
      Self->OnStop.clear();
   }
   else if (Self->OnStop.isC()) {
      kt::SwitchContext context(Self->OnStop.Context);
      auto routine = (void (*)(extSound *, APTR))Self->OnStop.Routine;
      routine(Self, Self->OnStop.Meta);
   }
   else if (Self->OnStop.isScript()) {
      sc::Call(Self->OnStop, std::to_array<ScriptArg>({ { "Sound", Self, FD_OBJECTPTR } }));
   }
}

//********************************************************************************************************************

static int read_stream(int Handle, int64_t Offset, APTR Buffer, int Length)
{
   auto Self = (extSound *)CurrentContext();

   if (Length > 0) {
      // Client-side producer seeks must not restart the consuming mixer channel.

      Self->FeedingStream = true;
      if ((Offset >= 0) and (Self->Position != Offset)) Self->seekStart(Offset);
      Self->FeedingStream = false;

      int result;
      Self->read(std::span<int8_t>((int8_t *)Buffer, Length), &result);
      return result;
   }

   return 0;
}

static void onstop_event(int SampleHandle)
{
   sound_stopped_event((extSound *)CurrentContext());
}

//********************************************************************************************************************

[[maybe_unused]] static ERR snd_init_audio(extSound *Self)
{
   kt::Log log;

   if (!FindObject("SystemAudio", CLASSID::AUDIO, &Self->AudioID)) return ERR::Okay;

   extAudio *audio;
   ERR error;
   if (!(error = NewObject(CLASSID::AUDIO, &audio))) {
      SetName(audio, "SystemAudio");
      SetOwner(audio, CurrentTask());

      if (!InitObject(audio)) {
         if (!(error = audio->activate())) {
            Self->AudioID = audio->UID;
         }
         else FreeResource(audio);
      }
      else {
         FreeResource(audio);
         error = ERR::Init;
      }
   }
   else if (error IS ERR::ObjectExists) return ERR::Okay;
   else error = ERR::NewObject;

   if (error != ERR::Okay) return log.warning(ERR::CreateObject);

   return error;
}

//********************************************************************************************************************

static int sound_frame_size(extSound *Self)
{
   return (((Self->Flags & SDF::STEREO) != SDF::NIL) ? 2 : 1) * (Self->BitsPerSample>>3);
}

//********************************************************************************************************************

static double sound_seconds(extSound *Self, int64_t Position)
{
   const int frame_size = sound_frame_size(Self);
   const int rate = Self->Playback ? Self->Playback : Self->Frequency;

   if ((frame_size <= 0) or (rate <= 0)) return 0;
   else return double(Position) / double(frame_size) / double(rate);
}

//********************************************************************************************************************

static int64_t clamp_sound_position(extSound *Self, int64_t Position)
{
   if (Position < 0) return 0;
   else if ((Self->Length > 0) and (Position > Self->Length)) return Self->Length;
   else return Position;
}

//********************************************************************************************************************

static ERR sound_play_position(extSound *Self, int64_t *Value)
{
   if (!Value) return ERR::NullArgs;

   *Value = clamp_sound_position(Self, Self->Position);

   if (Self->Length <= 0) return ERR::FieldNotSet;

   if ((Self->ChannelIndex) and (Self->AudioID)) {
      kt::ScopedObjectLock<extAudio> audio(Self->AudioID);
      if (audio.granted()) {
         std::lock_guard mixer_lock(audio->MixerMutex);
         #ifdef AUDIO_WORKER
         flush_audio_commands(*audio);
         #endif
         if (auto channel = audio->GetChannel(Self->ChannelIndex)) {
            if ((channel->SampleHandle IS Self->Handle) and (channel->State != CHS::STOPPED)) {
               auto &sample = audio->Samples[Self->Handle];
               const int shift = sample_shift(sample.SampleType);
               int64_t position = (int64_t(channel->Position) << shift) +
                  ((int64_t(channel->PositionLow) << shift) >> 16);

               if (sample.Stream) {
                  #ifdef AUDIO_WORKER
                  position = sample.PlayPos;
                  #else
                  position = int64_t(sample.PlayPos) - int64_t(sample.BufferedLength) + position;
                  #endif
               }

               #ifdef _WIN32
               const int64_t queued = int64_t(audio->MixerLag() * channel->Frequency) << shift;
               position = std::max(int64_t(0), position - queued);
               #endif
               *Value = clamp_sound_position(Self, position);
            }
         }
      }
      else return ERR::AccessObject;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Activate: Plays the audio sample.

Calling Activate will play the sample data from the current seek position defined by the #Position field.  Playback
continues asynchronously and the client can monitor its progress through the #Position field, or receive an event
notification through an #OnStop callback once playback has stopped.

The #Length field must be set prior to activation, otherwise `ERR::FieldNotSet` is returned.

On first activation the sample data is either loaded into an audio buffer in its entirety, or configured for streaming
according to the #Stream field.  Streaming is enabled automatically when `STREAM::ALWAYS` is set and the sample exceeds
16KB, or when `STREAM::SMART` is set and the sample exceeds 256KB.  Subsequent calls reuse the prepared buffer.

If the `LOOP` flag is defined then the sample will loop continuously between #LoopStart and #LoopEnd (or the full sample
length if #LoopEnd is zero) until deactivated.

The sound is assigned to the first available mixer channel.  If every channel is in use, a channel belonging to a sound
of lower #Priority may be reallocated for playback.  When no suitable channel can be obtained, `ERR::ArrayFull` is
returned.  Samples flagged with `RESTRICT_PLAY` or configured for streaming are limited to a single active channel, so
re-activating them will restart playback on that channel rather than mixing a second copy.

The current #Volume, #Pan and #Playback (frequency) values are applied to the channel when playback begins.  If the seek
position is at or beyond the end of the sample, it is automatically reset to the start before playing.
-END-
*********************************************************************************************************************/

static ERR SOUND_Activate(extSound *Self)
{
   kt::Log log;

   log.branch("Position: %" PF64 ", Active: %d", (long long)Self->Position, Self->Active);

   if (!Self->Length) return log.warning(ERR::FieldNotSet);

   if (Self->Active) {
      int active;
      if ((SOUND_GET_Active(Self, &active) IS ERR::Okay) and (!active)) Self->Active = false;
   }


   if ((!Self->Active) and (Self->Position >= Self->Length)) {
      if (Self->seekStart(0) != ERR::Okay) return log.warning(ERR::Seek);
   }

   if (!Self->Handle) {
      // Determine the sample type

      auto sampleformat = SFM::NIL;
      if (Self->BitsPerSample IS 8) {
         sampleformat = ((Self->Flags & SDF::STEREO) != SDF::NIL) ? SFM::U8_BIT_STEREO : SFM::U8_BIT_MONO;
      }
      else if (Self->BitsPerSample IS 16) {
         sampleformat = ((Self->Flags & SDF::STEREO) != SDF::NIL) ? SFM::S16_BIT_STEREO : SFM::S16_BIT_MONO;
      }

      if (sampleformat IS SFM::NIL) return log.warning(ERR::InvalidData);

      // Create the audio buffer and fill it with sample data

      if ((Self->Stream IS STREAM::ALWAYS) and (Self->Length > 16 * 1024)) Self->Flags |= SDF::STREAM;
      else if ((Self->Stream IS STREAM::SMART) and (Self->Length > 256 * 1024)) Self->Flags |= SDF::STREAM;

      if ((Self->Flags & SDF::STREAM) != SDF::NIL) {
         log.msg("Streaming enabled for playback in format $%.8x; Length: %" PF64, int(sampleformat),
            (long long)Self->Length);

         struct snd::AddStream stream;
         AudioLoop loop{};
         if ((Self->Flags & SDF::LOOP) != SDF::NIL) {
            loop.LoopMode   = LOOP::SINGLE;
            loop.Loop1Type  = LTYPE::UNIDIRECTIONAL;
            loop.Loop1Start = Self->LoopStart;
            if (Self->LoopEnd) loop.Loop1End = Self->LoopEnd;
            else loop.Loop1End = Self->Length;

            stream.Loop = &loop;
         }
         else stream.Loop = nullptr;

         if (Self->OnStop.defined()) stream.OnStop = C_FUNCTION(onstop_event);
         else stream.OnStop.clear();

         stream.PlayOffset   = Self->Position;
         stream.Callback     = C_FUNCTION(read_stream);
         stream.SampleFormat = sampleformat;
         stream.SampleLength = Self->Length;

         kt::ScopedObjectLock<extAudio> audio(Self->AudioID, 250);
         if (audio.granted()) {
            if (!Action(snd::AddStream::id, *audio, &stream)) {
               Self->Handle = stream.Result;
            }
            else {
               log.warning("Failed to add sample to the Audio device.");
               return ERR::ResourceRegistration;
            }
         }
         else return ERR::AccessObject;
      }
      else if (Self->Length > INT_MAX) return log.warning(ERR::OutOfRange);
      else if (void *buffer = malloc(size_t(Self->Length))) {
         auto dc = deferred_call([&buffer] { free(buffer); });

         auto client_pos = Self->Position;
         if (Self->Position) Self->seekStart(0); // Ensure we're reading the entire sample from the start

         int result;
         if (!Self->read(std::span<int8_t>((int8_t *)buffer, size_t(Self->Length)), &result)) {
            if (result != Self->Length) log.warning("Expected %" PF64 " bytes, read %d",
               (long long)Self->Length, result);

            Self->seekStart(client_pos);

            struct snd::AddSample add;
            AudioLoop loop{};

            if ((Self->Flags & SDF::LOOP) != SDF::NIL) {
               loop.LoopMode   = LOOP::SINGLE;
               loop.Loop1Type  = LTYPE::UNIDIRECTIONAL;
               loop.Loop1Start = Self->LoopStart;
               if (Self->LoopEnd) loop.Loop1End = Self->LoopEnd;
               else loop.Loop1End = Self->Length;

               add.Loop = &loop;
            }
            else add.Loop = nullptr;

            if (Self->OnStop.defined()) add.OnStop = C_FUNCTION(onstop_event);
            else add.OnStop.clear();

            add.SampleFormat = sampleformat;
            add.Data         = std::span<const int8_t>((int8_t *)buffer, size_t(Self->Length));

            kt::ScopedObjectLock<extAudio> audio(Self->AudioID, 250);
            if (audio.granted()) {
               if (!Action(snd::AddSample::id, *audio, &add)) {
                  Self->Handle = add.Result;
               }
               else {
                  log.warning("Failed to add sample to the Audio device.");
                  return ERR::ResourceRegistration;
               }
            }
            else return log.warning(ERR::AccessObject);
         }
         else return log.warning(ERR::Read);
      }
      else return log.warning(ERR::AllocMemory);
   }

   kt::ScopedObjectLock<extAudio> audio(Self->AudioID, 2000);
   if (audio.granted()) {
      std::lock_guard mixer_lock(audio->MixerMutex);
      #ifdef AUDIO_WORKER
      flush_audio_commands(*audio);
      #endif
      // Restricted and streaming audio can be played on only one channel at any given time.  This search will check
      // if the sound object is already active on one of our channels.

      AudioChannel *channel = nullptr;
      if ((Self->Flags & (SDF::RESTRICT_PLAY|SDF::STREAM)) != SDF::NIL) {
         Self->ChannelIndex &= 0xffff0000;
         int i;
         for (i=0; i < audio->MaxChannels; i++) {
            channel = audio->GetChannel(Self->ChannelIndex);
            if ((channel) and (channel->SampleHandle IS Self->Handle)) break;
            Self->ChannelIndex++;
         }
         if (i >= audio->MaxChannels) channel = nullptr;
      }

      if (!channel) {
         // Find an available channel.  If all channels are in use, check the priorities to see if we can push anyone out.
         AudioChannel *priority = nullptr;
         Self->ChannelIndex &= 0xffff0000;
         int i;
         for (i=0; i < audio->MaxChannels; i++) {
            if (auto candidate = audio->GetChannel(Self->ChannelIndex)) {
               if (candidate->isStopped() and !candidate->Paused) {
                  channel = candidate;
                  break;
               }
               else if ((candidate->Priority < Self->Priority) and
                        ((!priority) or (candidate->Priority < priority->Priority))) priority = candidate;
            }
            Self->ChannelIndex++;
         }

         if (i >= audio->MaxChannels) {
            if (!(channel = priority)) {
               log.msg("Audio channel not available for playback.");
               return ERR::ArrayFull;
            }
         }
      }

      Self->ChannelIndex = channel->Handle;
      channel->Priority = Self->Priority;
      snd::MixStop(*audio, Self->ChannelIndex);

      if (!snd::MixSample(*audio, Self->ChannelIndex, Self->Handle)) {
         if (snd::MixVolume(*audio, Self->ChannelIndex, Self->Volume) != ERR::Okay) return log.warning(ERR::AudioMix);
         if (snd::MixPan(*audio, Self->ChannelIndex, Self->Pan) != ERR::Okay) return log.warning(ERR::AudioMix);
         if (snd::MixFrequency(*audio, Self->ChannelIndex, Self->Playback) != ERR::Okay) return log.warning(ERR::AudioMix);
         if (snd::MixPlay(*audio, Self->ChannelIndex, Self->Position) != ERR::Okay) return log.warning(ERR::AudioMix);

         Self->Active = true;
         return ERR::Okay;
      }
      else {
         log.warning("Failed to set sample %d to channel $%.8x", Self->Handle, Self->ChannelIndex);
         return ERR::AudioMix;
      }
   }
   else return log.warning(ERR::AccessObject);
}

//********************************************************************************************************************

static void notify_onstop_free(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto self = (extSound *)CurrentContext();
   if (self->OnStop.defined()) self->OnStop.unpin();
   self->OnStop.clear();
}

/*********************************************************************************************************************
-ACTION-
Deactivate: Stops the audio sample and resets the playback position.
-END-
*********************************************************************************************************************/

static ERR SOUND_Deactivate(extSound *Self)
{
   kt::Log log;

   log.branch();


   Self->Position = 0;

   if (Self->ChannelIndex) {
      kt::ScopedObjectLock<extAudio> audio(Self->AudioID);
      if (audio.granted()) {
         std::lock_guard mixer_lock(audio->MixerMutex);
         #ifdef AUDIO_WORKER
         flush_audio_commands(*audio);
         #endif // Stop the sample if it's live.
         if (auto channel = audio->GetChannel(Self->ChannelIndex)) {
            if (channel->SampleHandle IS Self->Handle) snd::MixStop(*audio, Self->ChannelIndex);
         }
      }
      else return log.warning(ERR::AccessObject);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Disable: Disable playback of an active audio sample, equivalent to pausing.
-END-
*********************************************************************************************************************/

static ERR SOUND_Disable(extSound *Self)
{
   kt::Log log;

   log.branch();

   int64_t position;
   if (sound_play_position(Self, &position) IS ERR::Okay) Self->Position = position;

   if (!Self->ChannelIndex) return ERR::Okay;

   kt::ScopedObjectLock<extAudio> audio(Self->AudioID, 5000);
   if (audio.granted()) {
      std::lock_guard mixer_lock(audio->MixerMutex);
      #ifdef AUDIO_WORKER
      flush_audio_commands(*audio);
      #endif
      if (auto channel = audio->GetChannel(Self->ChannelIndex)) {
         if (channel->SampleHandle IS Self->Handle) snd::MixPause(*audio, Self->ChannelIndex);
      }
   }
   else return log.warning(ERR::AccessObject);

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Enable: Continues playing a sound if it has been disabled.
-END-
*********************************************************************************************************************/

static ERR SOUND_Enable(extSound *Self)
{
   kt::Log log;
   log.branch();

   if (!Self->ChannelIndex) return ERR::Okay;

   kt::ScopedObjectLock<extAudio> audio(Self->AudioID, 5000);
   if (audio.granted()) {
      std::lock_guard mixer_lock(audio->MixerMutex);
      #ifdef AUDIO_WORKER
      flush_audio_commands(*audio);
      #endif
      if (auto channel = audio->GetChannel(Self->ChannelIndex)) {
         if (channel->SampleHandle IS Self->Handle) snd::MixContinue(*audio, Self->ChannelIndex);
      }
   }
   else return log.warning(ERR::AccessObject);

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
GetKey: Retrieve custom key values.

The following custom key values are formally recognised and may be defined automatically when loading sample files:

<types type="Tag">
<type name="Author">The name of the person or organisation that created the sound sample.</type>
<type name="Copyright">Copyright details of an audio sample.</type>
<type name="Description">Long description for an audio sample.</type>
<type name="Disclaimer">The disclaimer associated with an audio sample.</type>
<type name="Software">The name of the application that was used to record the audio sample.</type>
<type name="Title">The title of the audio sample.</type>
<type name="Quality">The compression quality value if the source is an MP3 stream.</type>
</types>

*********************************************************************************************************************/

static ERR SOUND_GetKey(extSound *Self, struct acGetKey *Args)
{
   if ((not Args) or (not Args->Value)) return ERR::NullArgs;

   std::string name(Args->Key);
   if (Self->Tags.contains(name)) {
      Args->Value->assign(Self->Tags[name]);
      return ERR::Okay;
   }
   else return ERR::UnsupportedField;
}

/*********************************************************************************************************************
-ACTION-
Init: Prepares a sound object for usage.
-END-
*********************************************************************************************************************/

static ERR SOUND_Init(extSound *Self)
{
   kt::Log log;
   int id, len, result, pos;
   ERR error;

   if (!Self->AudioID) {
      if ((error = snd_init_audio(Self)) != ERR::Okay) return error;
   }

   // Open channels for sound sample playback.

   if (!(Self->ChannelIndex = glSoundChannels[Self->AudioID])) {
      kt::ScopedObjectLock<extAudio> audio(Self->AudioID, 3000);
      if (audio.granted()) {
         std::lock_guard mixer_lock(audio->MixerMutex);
         #ifdef AUDIO_WORKER
         flush_audio_commands(*audio);
         #endif
         if (!audio->openChannels(audio->MaxChannels, &Self->ChannelIndex)) {
            glSoundChannels[Self->AudioID] = Self->ChannelIndex;
         }
         else {
            log.warning("Failed to open audio channels.");
            return ERR::CreateResource;
         }
      }
      else return log.warning(ERR::AccessObject);
   }

   std::string_view path;
   Self->getPath(path);

   if (((Self->Flags & SDF::NEW) != SDF::NIL) or (path.empty())) {
      log.msg("Sample created as new (without sample data).");

      // If the sample is new or no path has been specified, create an audio sample from scratch (e.g. to
      // record audio to disk).

      return ERR::Okay;
   }

   // Load the sound file's header and test it to see if it matches our supported file format.

   if (!Self->File) {
      auto file = objFile::create::local(fl::Path(path), fl::Flags(FL::READ|FL::APPROXIMATE));
      if (!file) {
         return log.warning(ERR::File);
      }
      Self->File.reset(file);
   }
   else Self->File->seekStart(0);

   Self->File->read(std::span<int8_t>((int8_t *)Self->Header.data(), Self->Header.size()));

   if ((std::string_view((char *)Self->Header.data(), 4) != "RIFF") or
       (std::string_view((char *)Self->Header.data() + 8, 4) != "WAVE")) {
      Self->File.reset();
      return ERR::NoSupport;
   }

   // Read the FMT header

   Self->File->seek(12, SEEK::START);
   if (fl::ReadLE(Self->File.get(), &id) != ERR::Okay) return ERR::Read; // Contains the characters "fmt "
   if (fl::ReadLE(Self->File.get(), &len) != ERR::Okay) return ERR::Read; // Length of data in this chunk

   WAVEFormat WAVE;
   if ((Self->File->read(std::span<int8_t>((int8_t *)&WAVE, len), &result) != ERR::Okay) or (result < len)) {
      log.warning("Failed to read WAVE format header (got %d, expected %d)", result, len);
      return ERR::Read;
   }

   // Check the format of the sound file's data

   if ((WAVE.Format != WAVE_ADPCM) and (WAVE.Format != WAVE_RAW)) {
      log.warning("This file's WAVE data format is not supported (type %d).", WAVE.Format);
      return ERR::InvalidData;
   }

   // TODO Look for the cue chunk for loop information

   int64_t file_pos;
   Self->File->getPosition(file_pos);
   pos = int(file_pos);
#if 0
   if (!find_chunk(Self->File.get(), "cue ")) {
      data_p += 32;
      fl::ReadLE(Self->File, &info.loopstart);
      // if the next chunk is a LIST chunk, look for a cue length marker
      if (!find_chunk(Self->File.get(), "LIST")) {
         if (!strncmp (data_p + 28, "mark", 4)) {
            data_p += 24;
            fl::ReadLE(Self->File, &i);	// samples in loop
            info.samples = info.loopstart + i;
         }
      }
   }
#endif

   Self->File->seekStart(pos);

   // Look for the "data" chunk

   if (find_chunk(Self->File.get(), "data") != ERR::Okay) {
      return log.warning(ERR::Read);
   }

   // Setup the sound structure

   uint32_t data_length;
   fl::ReadLE(Self->File.get(), &data_length); // Length of audio data in this chunk
   Self->Length = data_length;

   Self->File->getPosition(file_pos);
   Self->DataOffset = int(file_pos);

   Self->Format         = WAVE.Format;
   Self->BytesPerSecond = WAVE.AvgBytesPerSecond;
   Self->BitsPerSample  = WAVE.BitsPerSample;
   if (WAVE.Channels IS 2)   Self->Flags |= SDF::STEREO;
   if (Self->Frequency <= 0) Self->Frequency = WAVE.Frequency;
   if (Self->Playback <= 0)  Self->Playback  = Self->Frequency;

   if ((Self->Flags & SDF::NOTE) != SDF::NIL) {
      SOUND_SET_Note(Self, Self->NoteString);
      Self->Flags &= ~SDF::NOTE;
   }

   if ((Self->BitsPerSample != 8) and (Self->BitsPerSample != 16)) {
      log.warning("Bits-Per-Sample of %d not supported.", Self->BitsPerSample);
      return ERR::InvalidData;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Read: Read decoded audio from the sound sample.

This action will read decoded audio from the sound sample.  Decoding is a live process and it may take some time for
all data to be returned if the requested amount of data is considerable.  The starting point for the decoded data
is determined by the #Position value.

-END-
*********************************************************************************************************************/

static ERR SOUND_Read(extSound *Self, struct acRead *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.traceBranch("Length: %" PRIu64 ", Offset: %" PF64, uint64_t(Args->Buffer.size()),
      (long long)Self->Position);

   if (Args->Buffer.empty()) {
      Args->Result = 0;
      return ERR::Okay;
   }

   if (not Args->Buffer.data()) return log.warning(ERR::NullArgs);

   // Don't read more than the known raw sample length

   int result;
   if (Self->Position >= Self->Length) {
      Args->Result = 0;
      return ERR::Okay;
   }
   auto read_size = std::min<size_t>(Args->Buffer.size(), size_t(Self->Length - Self->Position));
   if (auto error = Self->File->read(Args->Buffer.first(read_size), &result); error != ERR::Okay) return error;

   Self->Position += result;
   Args->Result = result;

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SaveToObject: Saves audio sample data to an object.
-END-
*********************************************************************************************************************/

static ERR SOUND_SaveToObject(extSound *Self, struct acSaveToObject *Args)
{
   kt::Log log;

   // Divert this call if the developer is trying to save the sound data as a specific derived type.

   if ((Args->ClassID != CLASSID::NIL) and (Args->ClassID != CLASSID::SOUND)) {
      auto mclass = (objMetaClass *)FindClass(Args->ClassID);

      std::span<ActionEntry> actions;
      if ((!mclass->getActionTable(actions)) and (not actions.empty())) {
         if (actions[int(AC::SaveToObject)].PerformAction) {
            return actions[int(AC::SaveToObject)].PerformAction(Self, Args);
         }
         else return log.warning(ERR::NoSupport);
      }
      else return log.warning(ERR::GetField);
   }

   if (!Self->Length) return log.warning(ERR::FieldNotSet);

   struct {
      char    ChunkID[4] = { 'R', 'I', 'F', 'F' };
      int     ChunkSize;      // File size - 8
      char    Format[4] = { 'W', 'A', 'V', 'E' };
      char    FmtChunkID[4] = { 'f', 'm', 't', ' ' };
      int     FmtChunkSize = 16;  // Size of format chunk (16 for PCM)
      int16_t AudioFormat = 1;    // 1 for PCM
      int16_t NumChannels;    // Number of channels
      int     SampleRate;     // Sample rate
      int     ByteRate;       // Byte rate
      int16_t BlockAlign;     // Block align
      int16_t BitsPerSample;  // Bits per sample
      char    DataChunkID[4] = { 'd', 'a', 't', 'a' }; // "data"
      int     DataChunkSize;  // Size of data
   } header;

   header.NumChannels   = ((Self->Flags & SDF::STEREO) != SDF::NIL) ? 2 : 1;
   header.SampleRate    = Self->Frequency;
   header.BitsPerSample = Self->BitsPerSample;
   header.BlockAlign    = (header.NumChannels * header.BitsPerSample) / 8;
   header.ByteRate      = header.SampleRate * header.BlockAlign;

   if (Self->Length > INT_MAX) return log.warning(ERR::OutOfRange);
   const int audio_data_size = Self->Length;

   header.DataChunkSize = audio_data_size;
   header.ChunkSize     = 36 + audio_data_size; // Header size (44) - 8 + data size

   if (acWrite(Args->Dest, std::span<const int8_t>((const int8_t *)&header, sizeof(header))) != ERR::Okay) {
      return log.warning(ERR::Write);
   }

   // Read and write audio data in chunks
   const int chunk_size = 8192;
   int bytes_remaining = audio_data_size;
   int original_position = Self->Position;

   if (Self->seekStart(0) != ERR::Okay) return log.warning(ERR::Seek);

   while (bytes_remaining > 0) {
      int read_size = (bytes_remaining < chunk_size) ? bytes_remaining : chunk_size;
      uint8_t buffer[8192];
      int bytes_read;
      if (acRead(Self, std::span<int8_t>((int8_t *)buffer, read_size), &bytes_read) != ERR::Okay) {
         return log.warning(ERR::Read);
      }
      if (acWrite(Args->Dest, std::span<const int8_t>((const int8_t *)buffer, bytes_read)) != ERR::Okay) {
         return log.warning(ERR::Write);
      }
      bytes_remaining -= bytes_read;
      if (bytes_read < read_size) break;
   }

   Self->seekStart(original_position);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Seek: Moves the cursor position for reading data.

Use Seek to move the read cursor within the decoded audio stream and update #Position.  This will affect the
Read action.  If the sample is in active playback at the time of the call, the playback position will also be moved.

-END-
*********************************************************************************************************************/

static ERR SOUND_Seek(extSound *Self, struct acSeek *Args)
{
   kt::Log log;

   // NB: Derived -classes may divert their functionality to this routine if the sample is fully buffered.

   if (!Args) return log.warning(ERR::NullArgs);
   if (!Self->initialised()) return log.warning(ERR::NotInitialised);

   if (Args->Position IS SEEK::START)         Self->Position = int64_t(Args->Offset);
   else if (Args->Position IS SEEK::END)      Self->Position = Self->Length - int64_t(Args->Offset);
   else if (Args->Position IS SEEK::CURRENT)  Self->Position += int64_t(Args->Offset);
   else if (Args->Position IS SEEK::RELATIVE) Self->Position = Self->Length * Args->Offset;
   else return log.warning(ERR::Args);

   if (Self->Position < 0) Self->Position = 0;
   else if (Self->Position > Self->Length) Self->Position = Self->Length;
   else { // Retain correct byte alignment.
      int align = ((((Self->Flags & SDF::STEREO) != SDF::NIL) ? 2 : 1) * (Self->BitsPerSample>>3)) - 1;
      Self->Position &= ~align;
   }

   log.traceBranch("Seek to %" PF64 " + %d", (long long)Self->Position, Self->DataOffset);

   if ((Self->File) and (!Self->isDerived())) {
      Self->File->seekStart(Self->DataOffset + Self->Position);
   }

   kt::ScopedObjectLock<extAudio> audio(Self->AudioID, 2000);
   if (audio.granted()) {
      std::lock_guard mixer_lock(audio->MixerMutex);
      #ifdef AUDIO_WORKER
      flush_audio_commands(*audio);
      #endif
         if (Self->Handle and !Self->FeedingStream) {
            audio->Samples[Self->Handle].PlayPos = BYTELEN(Self->Position);

            if (Self->Active) {
               // Adjust the playback position now if the sample is in playback.  Streams are restarted so that the
               // buffer is refilled from the new position and the channel's anticipated end-time is recomputed;
               // otherwise stale buffered audio continues to play and the OnStop event is mistimed.

               if (Self->ChannelIndex) {
                  if (auto channel = audio->GetChannel(Self->ChannelIndex)) {
                     if (!channel->isStopped()) {
                        snd::MixPlay(*audio, Self->ChannelIndex, Self->Position);
                     }
                  }
               }
            }
         }
   }
   else return log.warning(ERR::AccessObject);

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SetKey: Define custom tags that will be saved with the sample data.
-END-
*********************************************************************************************************************/

static ERR SOUND_SetKey(extSound *Self, struct acSetKey *Args)
{
   if ((!Args) or (Args->Key.empty())) return ERR::NullArgs;

   Self->Tags[std::string(Args->Key)] = Args->Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Active: Returns `true` if the sound sample is being played back.
-END-
*********************************************************************************************************************/

static ERR SOUND_GET_Active(extSound *Self, int *Value)
{
   *Value = FALSE;

   if (Self->ChannelIndex) {
      kt::ScopedObjectLock<extAudio> audio(Self->AudioID);
      if (audio.granted()) {
         std::lock_guard mixer_lock(audio->MixerMutex);
         #ifdef AUDIO_WORKER
         flush_audio_commands(*audio);
         #endif
         if (auto channel = audio->GetChannel(Self->ChannelIndex)) {
            if (!channel->isStopped()) *Value = TRUE;
         }
      }
      else return ERR::AccessObject;
   }

   Self->Active = *Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Audio: Refers to the audio object/device to use for playback.

Set this field if a specific @Audio object should be targeted when playing the sound sample.

-FIELD-
BitsPerSample: Indicates the sample rate of the audio sample, typically `8` or `16` bit.

-FIELD-
BytesPerSecond: The flow of bytes-per-second when the sample is played at normal frequency.

This field is set on initialisation.  It indicates the total number of bytes per second that will be played if the
sample is played back at its normal frequency.

-FIELD-
ChannelIndex: Refers to the channel that the sound is playing through.

This field reflects the audio channel index that the sound is currently playing through, or has most recently played
through.

-FIELD-
Compression: Determines the amount of compression used when saving an audio sample.

Setting the Compression field will determine how much compression is applied when saving an audio sample.  The range of
compression is 0 to 100%, with 100% being the strongest level available while 0% is uncompressed and loss-less.  This
field is ignored if the file format does not support compression.

-FIELD-
Duration: Returns the duration of the sample, measured in seconds.

*********************************************************************************************************************/

static ERR SOUND_GET_Duration(extSound *Self, double *Value)
{
   if (Self->Length) {
      const int bytes_per_sample = ((((Self->Flags & SDF::STEREO) != SDF::NIL) ? 2 : 1) * (Self->BitsPerSample>>3));
      *Value = double(Self->Length / bytes_per_sample) / double(Self->Playback ? Self->Playback : Self->Frequency);
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

/*********************************************************************************************************************

-FIELD-
Elapsed: Returns the elapsed playback time, measured in seconds.

This field reports the current playback position in seconds.  It is derived from the live playback channel when the
sample is active, or from the stored #Position field when playback is stopped.

*********************************************************************************************************************/

static ERR SOUND_GET_Elapsed(extSound *Self, double *Value)
{
   int64_t position;
   if (auto error = sound_play_position(Self, &position); error != ERR::Okay) return error;

   *Value = sound_seconds(Self, position);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Flags: Optional initialisation flags.
Lookup: SDF

*********************************************************************************************************************/

static ERR SOUND_SET_Flags(extSound *Self, int Value)
{
   Self->Flags = SDF((int(Self->Flags) & 0xffff0000) | (Value & 0x0000ffff));
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Frequency: The frequency of a sampled sound is specified here.

This field specifies the frequency of the sampled sound data.  If the frequency cannot be determined from the source,
this value will be zero.

Note that if the playback frequency needs to be altered, set the #Playback field.

-FIELD-
Header: Contains the first 128 bytes of data in a sample's file header.

The Header field is a pointer to a 128 byte buffer that contains the first 128 bytes of information read from an audio
file on initialisation.  This special field is considered to be helpful only to developers writing add-on components
for the sound class.

The buffer that is referred to by the Header field is not populated until the Init action is called on the sound object.

*********************************************************************************************************************/

static ERR SOUND_GET_Header(extSound *Self, std::span<uint8_t> &Array)
{
   Array = Self->Header;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Length: Indicates the total byte-length of sample data.

This field specifies the length of the sample data in bytes.  To get the length of the sample in seconds, divide this
value by the #BytesPerSecond field.

*********************************************************************************************************************/

static ERR SOUND_SET_Length(extSound *Self, int64_t Value)
{
   kt::Log log;
   if (Value >= 0) {
      Self->Length = Value;

         if ((Self->Handle) and (Self->AudioID)) {
            kt::ScopedObjectLock<extAudio> audio(Self->AudioID);
            if (audio.granted()) {
               std::lock_guard mixer_lock(audio->MixerMutex);
               #ifdef AUDIO_WORKER
               flush_audio_commands(*audio);
               #endif
               return audio->setSampleLength(Self->Handle, Value);
            }
            else return log.warning(ERR::AccessObject);
         }
         else return ERR::Okay;
   }
   else return log.warning(ERR::InvalidValue);
}

/*********************************************************************************************************************

-FIELD-
LoopEnd: The byte position at which sample looping will end.

When using looped samples (via the `SDF::LOOP` flag), set the LoopEnd field if the sample should end at a position
that is earlier than the sample's actual length.  The LoopEnd value is specified in bytes and must be less or equal
to the length of the sample and greater than the #LoopStart value.

-FIELD-
LoopStart: The byte position at which sample looping begins.

When using looped samples (via the `SDF::LOOP` flag), set the LoopStart field if the sample should begin at a position
other than zero.  The LoopStart value is specified in bytes and must be less than the length of the sample and the
#LoopEnd value.

Note that the LoopStart variable does not affect the position at which playback occurs for the first time - it only
affects the restart position when the end of the sample is reached.

-FIELD-
Note: The musical note to use when playing a sound sample.
Lookup: NOTE

Set the Note field to alter the playback frequency of a sound sample.  Setting this field as opposed
to the #Playback frequency will assure that the sample is played at a correctly scaled tone.

The Note field can be set using either string or integer based format.  If using the integer format, the chosen
value will reflect the position on a musical keyboard.  A value of zero refers to the middle C key.  Each
octave is measured in sets of 12 notes, so a value of 24 would indicate a C note at 3 times normal playback.  To play
at lower values, simply choose a negative integer to slow down sample playback.

Setting the Note field with the string format is useful if human readability is valuable.  The correct format
is `KEY OCTAVE SHARP`.  Here are some examples: `C5, D7#, G2, E3S`.

The middle C key for this format is `C5`.  The maximum octave that you can achieve for the string format is 9
and the lowest is 0.  Use either the `S` character or the `#` character for referral to a sharp note.

*********************************************************************************************************************/

static ERR SOUND_GET_Note(extSound *Self, std::string_view &Value)
{
   bool sharp = false;

   switch(Self->Note) {
      case NOTE_C:  Self->NoteString = "C"; break;
      case NOTE_CS: Self->NoteString = "C"; sharp = true; break;
      case NOTE_D:  Self->NoteString = "D"; break;
      case NOTE_DS: Self->NoteString = "D"; sharp = true; break;
      case NOTE_E:  Self->NoteString = "E"; break;
      case NOTE_F:  Self->NoteString = "F"; break;
      case NOTE_FS: Self->NoteString = "F"; sharp = true; break;
      case NOTE_G:  Self->NoteString = "G"; break;
      case NOTE_GS: Self->NoteString = "G"; sharp = true; break;
      case NOTE_A:  Self->NoteString = "A"; break;
      case NOTE_AS: Self->NoteString = "A"; sharp = true; break;
      case NOTE_B:  Self->NoteString = "B"; break;
      default:      Self->NoteString.clear();
                    Value = Self->NoteString;
                    return ERR::FieldNotSet;
   }

   Self->NoteString += char('5' + Self->Octave);
   if (sharp) Self->NoteString += '#';

   Value = Self->NoteString;
   return ERR::Okay;
}

static ERR SOUND_SET_Note(extSound *Self, const std::string_view &Value)
{
   kt::Log log;

   if (Value.empty()) return ERR::Okay;

   int i, note;
   Self->NoteString.assign(Value);

   const char *str = Self->NoteString.c_str();
   if (((*str >= '0') and (*str <= '9')) or (*str IS '-')) {
      note = strtol(str, nullptr, 0);
   }
   else {
      note = 0;
      switch (*str) {
         case 'C': case 'c': note = NOTE_C; break;
         case 'D': case 'd': note = NOTE_D; break;
         case 'E': case 'e': note = NOTE_E; break;
         case 'F': case 'f': note = NOTE_F; break;
         case 'G': case 'g': note = NOTE_G; break;
         case 'A': case 'a': note = NOTE_A; break;
         case 'B': case 'b': note = NOTE_B; break;
         default:  note = NOTE_C;
      }
      str++;
      if ((*str >= '0') and (*str <= '9')) {
         note += NOTE_OCTAVE * (*str - '5');
         str++;
      }
      if ((*str IS 'S') or (*str IS 's') or (*str IS '#')) note++; // Sharp note
   }

   if ((note > NOTE_OCTAVE * 5) or (note < -(NOTE_OCTAVE * 5))) return log.warning(ERR::OutOfRange);

   Self->Flags |= SDF::NOTE;

   // Calculate the note value

   if ((Self->Note = note) < 0) Self->Note = -Self->Note;
   Self->Note = Self->Note % NOTE_OCTAVE;
   if (Self->Note > NOTE_B) Self->Note = NOTE_B;

   // Calculate the octave value if the note is set outside of the normal range

   if (note < 0) Self->Octave = (note / NOTE_OCTAVE) - 1;
   else if (note > NOTE_B) Self->Octave = note / NOTE_OCTAVE;

   if (Self->Octave < -5) Self->Octave = -5;
   else if (Self->Octave > 5) Self->Octave = 5;

   // Return if there is no frequency setting yet

   if (!Self->Frequency) return ERR::Okay;

   // Get the default frequency and adjust it to suit the requested octave/scale
   Self->Playback = Self->Frequency;
   if (Self->Octave > 0) {
      for (i=0; i < Self->Octave; i++) Self->Playback = Self->Playback<<1;
   }
   else if (Self->Octave < 0) {
      for (i=0; i > Self->Octave; i--) Self->Playback = Self->Playback>>1;
   }

   // Tune the playback frequency to match the requested note

   Self->Playback = (int)(Self->Playback * glScale[Self->Note]);

   // If the sound is playing, set the new playback frequency immediately

   if (Self->ChannelIndex) {
      kt::ScopedObjectLock<extAudio> audio(Self->AudioID, 200);
      if (audio.granted()) {
         std::lock_guard mixer_lock(audio->MixerMutex);
         #ifdef AUDIO_WORKER
         flush_audio_commands(*audio);
         #endif
         snd::MixFrequency(*audio, Self->ChannelIndex, Self->Playback);
      }
      else return ERR::AccessObject;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Octave: The octave to use for sample playback.

The Octave field determines the octave to use when playing back a sound sample.  The default setting is zero, which
represents the octave at which the sound was sampled.  Setting a negative octave will lower the playback rate, while
positive values raise the playback rate.  The minimum octave setting is `-5` and the highest setting is `+5`.

The octave can also be adjusted by setting the #Note field.  Setting the Octave field directly is useful if
you need to quickly double or halve the playback rate.

*********************************************************************************************************************/

static ERR SOUND_SET_Octave(extSound *Self, int Value)
{
   if ((Value < -10) or (Value > 10))
   Self->Octave = Value;
   if (auto field = FindField(Self, strhash("note"), nullptr)) return Self->set(field, Self->Note);
   else return ERR::SetField;
}

/*********************************************************************************************************************

-FIELD-
OnStop: This callback is triggered when sample playback stops.

Set OnStop to a callback function to receive an event trigger when sample playback stops.  The prototype for the
function is `void OnStop(*Sound)`.

The timing of this event does not guarantee precision, but should be accurate to approximately 1/100th of a second
in most cases.

*********************************************************************************************************************/

static ERR SOUND_GET_OnStop(extSound *Self, FUNCTION * &Value)
{
   if (Self->OnStop.defined()) {
      Value = &Self->OnStop;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SOUND_SET_OnStop(extSound *Self, FUNCTION *Value)
{
   if (Self->OnStop.defined()) {
      if (Self->OnStop.isScript()) UnsubscribeAction(Self->OnStop.Context, AC::Free);
      Self->OnStop.unpin();
      Self->OnStop.disable();
   }

   if (Value) {
      Self->OnStop = *Value;
      if (Self->OnStop.defined()) {
         Self->OnStop.pin();
         if (Self->OnStop.isScript()) {
            SubscribeAction(Self->OnStop.Context, AC::Free, C_FUNCTION(notify_onstop_free));
         }
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Pan: Determines the horizontal position of a sound when played through stereo speakers.

The Pan field adjusts the "horizontal position" of a sample that is being played through stereo speakers.
The default value for this field is zero, which plays the sound through both speakers at an equal level.  The minimum
value is `-1.0` to force play through the left speaker and the maximum value is `1.0` for the right speaker.

*********************************************************************************************************************/

static ERR SOUND_SET_Pan(extSound *Self, double Value)
{
   Self->Pan = Value;

   if (Self->Pan < -1.0) Self->Pan = -1.0;
   else if (Self->Pan > 1.0) Self->Pan = 1.0;

   if (Self->ChannelIndex) {
      kt::ScopedObjectLock<extAudio> audio(Self->AudioID, 200);
      if (audio.granted()) {
         std::lock_guard mixer_lock(audio->MixerMutex);
         #ifdef AUDIO_WORKER
         flush_audio_commands(*audio);
         #endif
         snd::MixPan(*audio, Self->ChannelIndex, Self->Pan);
      }
      else return ERR::AccessObject;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Path: Location of the audio sample data.

This field must refer to a file that contains the audio data that will be loaded.  If creating a new sample
with the `SDF::NEW` flag, it is not necessary to define a file source.

-FIELD-
Playback: The playback frequency of the sound sample can be defined here.

Set this field to define the exact frequency of a sample's playback.  The playback frequency can be modified at
any time, including during audio playback if real-time adjustments to a sample's audio output rate is desired.

*********************************************************************************************************************/

static ERR SOUND_SET_Playback(extSound *Self, int Value)
{
   kt::Log log;

   if ((Value < 0) or (Value > 192000)) return ERR::OutOfRange;

   Self->Playback = Value;
   Self->Flags &= ~SDF::NOTE;

   if (Self->ChannelIndex) {
      kt::ScopedObjectLock<extAudio> audio(Self->AudioID, 200);
      if (audio.granted()) {
         std::lock_guard mixer_lock(audio->MixerMutex);
         #ifdef AUDIO_WORKER
         flush_audio_commands(*audio);
         #endif
         snd::MixFrequency(*audio, Self->ChannelIndex, Self->Playback);
      }
      else return log.warning(ERR::AccessObject);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
PlayPosition: Returns the current playback position, measured in bytes.

This field differs from #Position because it reflects the live playback cursor while the sound is active.  For streamed
playback this is a best-effort source offset derived from the rolling stream buffer.  Reads are synchronised with
the mixer and may apply pending individual mixer commands.  ALSA reports the rendered source position, ahead of
audible output by the device queue.  Windows subtracts the estimated queued output duration to approximate audible
playback; this is not a hardware presentation timestamp.  Use the ~MixSubmitBatch() completion callback to acknowledge
batch execution independently of playback position.

*********************************************************************************************************************/

static ERR SOUND_GET_PlayPosition(extSound *Self, int64_t *Value)
{
   return sound_play_position(Self, Value);
}

/*********************************************************************************************************************
-FIELD-
Position: The current playback position.

The current playback position of the audio sample is indicated by this field.  Writing to the field will alter the
playback position, either when the sample is next played, or immediately if it is currently playing.

*********************************************************************************************************************/

static ERR SOUND_SET_Position(extSound *Self, int64_t Value)
{
   return Self->seekStart(Value);
}

/*********************************************************************************************************************

-FIELD-
Priority: The priority of a sound in relation to other sound samples being played.

The playback priority of the sample is defined here. This helps to determine if the sample should be played when all
available mixing channels are busy. Naturally, higher priorities are played over samples with low priorities.

The minimum priority value allowed is -100, the maximum is 100.

*********************************************************************************************************************/

static ERR SOUND_SET_Priority(extSound *Self, int Value)
{
   Self->Priority = Value;
   if (Self->Priority < -100) Self->Priority = -100;
   else if (Self->Priority > 100) Self->Priority = 100;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Progress: Returns the current playback progress as a normalised value.

The returned value ranges from `0.0` at the beginning of the sample to `1.0` at the end of the sample.

*********************************************************************************************************************/

static ERR SOUND_GET_Progress(extSound *Self, double *Value)
{
   int64_t position;
   if (auto error = sound_play_position(Self, &position); error != ERR::Okay) return error;

   *Value = double(position) / double(Self->Length);
   if (*Value < 0) *Value = 0;
   else if (*Value > 1.0) *Value = 1.0;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Remaining: Returns the remaining playback time, measured in seconds.

*********************************************************************************************************************/

static ERR SOUND_GET_Remaining(extSound *Self, double *Value)
{
   int64_t position;
   if (auto error = sound_play_position(Self, &position); error != ERR::Okay) return error;

   *Value = sound_seconds(Self, Self->Length - position);
   if (*Value < 0) *Value = 0;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Stream: Defines the preferred streaming method for the sample.
Lookup: STREAM

The Stream field controls how the Sound class manages memory and playback for audio samples. This setting directly
influences memory consumption, playback latency, and system resource utilisation.

Available streaming modes:

!STREAM

Smart and Always streaming modes use significantly less memory for large samples, making them suitable for applications
that handle extensive audio libraries. Never streaming provides the fastest playback initiation but may exhaust
system memory with large or numerous samples.

Memory-resident samples (Never streaming) offer zero-latency playback initiation, whilst streamed samples may have
a brief delay as the initial buffer is populated. However, once streaming begins, there should be no perceptible
difference in audio quality or continuity.

The Smart streaming mode monitors system resources and sample characteristics to make optimal decisions. This mode
adapts to changing conditions and provides the best balance for most applications without requiring manual configuration.

-FIELD-
Volume: The volume to use when playing the sound sample.

The field specifies the volume of a sound in the range 0 - 1.0 (low to high).  Setting this field during sample
playback will dynamically alter the volume.
-END-

*********************************************************************************************************************/

static ERR SOUND_SET_Volume(extSound *Self, double Value)
{
   if (Value < 0) Value = 0;
   else if (Value > 1.0) Value = 1.0;
   Self->Volume = Value;

   if (Self->ChannelIndex) {
      kt::ScopedObjectLock<extAudio> audio(Self->AudioID, 200);
      if (audio.granted()) {
         std::lock_guard mixer_lock(audio->MixerMutex);
         #ifdef AUDIO_WORKER
         flush_audio_commands(*audio);
         #endif
         snd::MixVolume(*audio, Self->ChannelIndex, Self->Volume);
      }
      else return ERR::AccessObject;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR find_chunk(objFile *File, std::string_view ChunkName)
{
   while (true) {
      char chunk[4];
      int len;
      if ((File->read(std::span<int8_t>((int8_t *)chunk, sizeof(chunk)), &len) != ERR::Okay) or
          (len != sizeof(chunk))) {
         return ERR::Read;
      }

      if (ChunkName IS std::string_view(chunk, 4)) return ERR::Okay;

      fl::ReadLE(File, &len); // Length of data in this chunk
      File->seekCurrent(len);
   }
}

//********************************************************************************************************************


//********************************************************************************************************************

extSound::~extSound() {

   if (OnStop.defined()) {
      if (OnStop.isScript()) UnsubscribeAction(OnStop.Context, AC::Free);
      OnStop.unpin();
      OnStop.disable();
   }


   deactivate();

   if ((Handle) and (AudioID)) {
      kt::ScopedObjectLock<extAudio> audio(AudioID);
      if (audio.granted()) {
         std::lock_guard mixer_lock(audio->MixerMutex);
         #ifdef AUDIO_WORKER
         flush_audio_commands(*audio);
         #endif
         audio->removeSample(Handle);
         Handle = 0;
      }
   }

   File.reset();
}

//********************************************************************************************************************

#include "class_sound_def.c"

static const FieldArray clFields[] = {
   { "Path",           FDF_CPPSTRING|FDF_RI, nullptr, nullptr },
   { "Src",            FDF_SYNONYM },
   { "Volume",         FDF_DOUBLE|FDF_RW, nullptr, SOUND_SET_Volume },
   { "Pan",            FDF_DOUBLE|FDF_RW, nullptr, SOUND_SET_Pan },
   { "Position",       FDF_INT64|FDF_RW, nullptr, SOUND_SET_Position },
   { "Length",         FDF_INT64|FDF_RW, nullptr, SOUND_SET_Length },
   { "LoopStart",      FDF_INT64|FDF_RW },
   { "LoopEnd",        FDF_INT64|FDF_RW },
   { "Priority",       FDF_INT|FDF_RW, nullptr, SOUND_SET_Priority },
   { "Octave",         FDF_INT|FDF_RW, nullptr, SOUND_SET_Octave },
   { "Flags",          FDF_INTFLAGS|FDF_RW, nullptr, SOUND_SET_Flags, &clSoundFlags },
   { "Frequency",      FDF_INT|FDF_RI },
   { "Playback",       FDF_INT|FDF_RW, nullptr, SOUND_SET_Playback },
   { "Compression",    FDF_INT|FDF_RW },
   { "BytesPerSecond", FDF_INT|FDF_RW },
   { "BitsPerSample",  FDF_INT|FDF_RW },
   { "Audio",          FDF_OBJECTID|FDF_RI },
   { "Stream",         FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clSoundStream },
   { "Handle",         FDF_INT|FDF_SYSTEM|FDF_R },
   { "ChannelIndex",   FDF_INT|FDF_R },
   // Virtual fields
   { "Active",       FDF_VIRTUAL|FDF_INT|FDF_R,                     SOUND_GET_Active },
   { "Duration",     FDF_VIRTUAL|FDF_DOUBLE|FDF_R|FDF_PURE,         SOUND_GET_Duration },
   { "Elapsed",      FDF_VIRTUAL|FDF_DOUBLE|FDF_R,                  SOUND_GET_Elapsed },
   { "Header",       FDF_VIRTUAL|FDF_BYTE|FDF_ARRAY|FDF_R|FDF_PURE, SOUND_GET_Header },
   { "OnStop",       FDF_VIRTUAL|FDF_FUNCTION|FDF_RW|FDF_PURE,      SOUND_GET_OnStop, SOUND_SET_OnStop },
   { "PlayPosition", FDF_VIRTUAL|FDF_INT64|FDF_R,                   SOUND_GET_PlayPosition },
   { "Progress",     FDF_VIRTUAL|FDF_DOUBLE|FDF_R,                  SOUND_GET_Progress },
   { "Remaining",    FDF_VIRTUAL|FDF_DOUBLE|FDF_R,                  SOUND_GET_Remaining },
   { "Note",         FDF_VIRTUAL|FDF_CPPSTRING|FDF_RW,              SOUND_GET_Note, SOUND_SET_Note },
   END_FIELD
};

//********************************************************************************************************************

ERR add_sound_class(void)
{
   clSound = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::SOUND),
      fl::ClassVersion(VER_SOUND),
      fl::FileExtension("wav|wave|snd"),
      fl::FileDescription("Sound Sample"),
      fl::FileHeader("[0:$52494646][8:$57415645]"),
      fl::Icon("filetypes/audio"),
      fl::Name("Sound"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clSoundActions),
      fl::Fields(clFields),
      fl::Size(sizeof(extSound)),
      fl::Path(MOD_PATH));

   return clSound ? ERR::Okay : ERR::AddClass;
}

void free_sound_class(void)
{
   if (clSound) { FreeResource(clSound); clSound = 0; }
}
