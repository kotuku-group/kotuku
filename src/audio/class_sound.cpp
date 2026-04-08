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
local snd = obj.new('sound', {
   path = 'audio:samples/piano_c4.wav',
   note = 'C6',    -- Play two octaves higher
   volume = 0.8,   -- Reduce volume to 80%
   onStop = function(Sound)
      print('Playback completed')
      proc.signal()
   end
})

snd.acActivate()
proc.sleep()  -- Wait for completion
</pre>

-END-

**********************************************************************************************************************

NOTE: Ideally individual samples should always be played through the host's audio capabilities and not our internal
mixer.  Our mixer is buffered and therefore always has a delay, whereas the host drivers should be able to play
individual samples with more immediacy.

*********************************************************************************************************************/

#include <array>

constexpr int SECONDS_STREAM_BUFFER = 2;
constexpr int SIZE_RIFF_CHUNK = 12;

static ERR SOUND_GET_Active(extSound *, int *);

static ERR SOUND_SET_Note(extSound *, CSTRING);

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

static ERR find_chunk(extSound *, objFile *, std::string_view);
#ifdef USE_WIN32_PLAYBACK
static ERR win32_audio_stream(extSound *, int64_t, int64_t);
#endif

//********************************************************************************************************************
// Send a callback to the client when playback stops.

static void sound_stopped_event(extSound *Self)
{
   if (Self->OnStop.isC()) {
      pf::SwitchContext context(Self->OnStop.Context);
      auto routine = (void (*)(extSound *, APTR))Self->OnStop.Routine;
      routine(Self, Self->OnStop.Meta);
   }
   else if (Self->OnStop.isScript()) {
      sc::Call(Self->OnStop, std::to_array<ScriptArg>({ { "Sound", Self, FD_OBJECTPTR } }));
   }
}

//********************************************************************************************************************

#ifndef USE_WIN32_PLAYBACK
static int read_stream(int Handle, int Offset, APTR Buffer, int Length)
{
   auto Self = (extSound *)CurrentContext();

   if ((Offset >= 0) and (Self->Position != Offset)) Self->seekStart(Offset);

   if (Length > 0) {
      int result;
      Self->read(Buffer, Length, &result);
      return result;
   }

   return 0;
}

static void onstop_event(int SampleHandle)
{
   sound_stopped_event((extSound *)CurrentContext());
}

#endif

//********************************************************************************************************************
// Called when the estimated time for playback is over.

#ifdef _WIN32
static ERR timer_playback_ended(extSound *Self, int64_t Elapsed, int64_t CurrentTime)
{
   pf::Log log;
   log.trace("Sound streaming completed.");
   sound_stopped_event(Self);
   Self->PlaybackTimer = 0;
   // NB: We don't manually stop the audio streamer, it will automatically stop once buffers are clear.
   return ERR::Terminate;
}
#endif

//********************************************************************************************************************
// Configure a timer that will trigger when the playback is finished.  The Position cursor will be taken into account
// in determining playback length.

#ifdef USE_WIN32_PLAYBACK
static ERR set_playback_trigger(extSound *Self)
{
   if (Self->OnStop.defined()) {
      pf::Log log(__FUNCTION__);
      const int bytes_per_sample = ((((Self->Flags & SDF::STEREO) != SDF::NIL) ? 2 : 1) * (Self->BitsPerSample>>3));
      const double playback_time = double((Self->Length - Self->Position) / bytes_per_sample) / double(Self->Playback);
      if (playback_time < 0.01) {
         if (Self->PlaybackTimer) { UpdateTimer(Self->PlaybackTimer, 0); Self->PlaybackTimer = 0; }
         timer_playback_ended(Self, 0, 0);
      }
      else {
         log.trace("Playback time period set to %.2fs", playback_time);
         if (Self->PlaybackTimer) return UpdateTimer(Self->PlaybackTimer, playback_time + 0.01);
         else {
            return SubscribeTimer(playback_time + 0.01, C_FUNCTION(timer_playback_ended), &Self->PlaybackTimer);
         }
      }
   }
   return ERR::Okay;
}
#endif

#ifdef _WIN32
extern "C" void end_of_stream(OBJECTPTR Object, int BytesRemaining)
{
   if (Object->Class->BaseClassID IS CLASSID::SOUND) {
      auto Self = (extSound *)Object;
      if (Self->OnStop.defined()) {
         pf::Log log(__FUNCTION__);
         pf::SwitchContext context(Object);
         const int bytes_per_sample = ((((Self->Flags & SDF::STEREO) != SDF::NIL) ? 2 : 1) * (Self->BitsPerSample>>3));
         const double playback_time = (double(BytesRemaining / bytes_per_sample) / double(Self->Playback)) + 0.01;

         if (!Self->PlaybackTimer) {
            if (playback_time < 0.01) {
               timer_playback_ended(Self, 0, 0);
            }
            else {
               log.trace("Remaining time period set to %.2fs", playback_time);
               SubscribeTimer(playback_time, C_FUNCTION(timer_playback_ended), &Self->PlaybackTimer);
            }
         }
      }
   }
}
#endif

//********************************************************************************************************************
// Stubs.

[[maybe_unused]] static SFM sample_format(extSound *Self)
{
   if (Self->BitsPerSample IS 8) {
      if ((Self->Flags & SDF::STEREO) != SDF::NIL) return SFM::U8_BIT_STEREO;
      else return SFM::U8_BIT_MONO;
   }
   else if (Self->BitsPerSample IS 16) {
      if ((Self->Flags & SDF::STEREO) != SDF::NIL) return SFM::S16_BIT_STEREO;
      else return SFM::S16_BIT_MONO;
   }
   return SFM::NIL;
}

[[maybe_unused]] static ERR snd_init_audio(extSound *Self)
{
   pf::Log log;

   if (FindObject("SystemAudio", CLASSID::AUDIO, FOF::NIL, &Self->AudioID) IS ERR::Okay) return ERR::Okay;

   extAudio *audio;
   ERR error;
   if ((error = NewObject(CLASSID::AUDIO, &audio)) IS ERR::Okay) {
      SetName(audio, "SystemAudio");
      SetOwner(audio, CurrentTask());

      if (InitObject(audio) IS ERR::Okay) {
         if ((error = audio->activate()) IS ERR::Okay) {
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

/*********************************************************************************************************************
-ACTION-
Activate: Plays the audio sample.
-END-
*********************************************************************************************************************/

static ERR SOUND_Activate(extSound *Self)
{
   pf::Log log;

   log.traceBranch("Position: %" PF64, (long long)Self->Position);

   if (!Self->Length) return log.warning(ERR::FieldNotSet);

#ifdef USE_WIN32_PLAYBACK
   // Optimised playback for Windows - this does not use our internal mixer.

   if (!Self->Active) {
      int16_t channels = ((Self->Flags & SDF::STEREO) != SDF::NIL) ? 2 : 1;
      WAVEFORMATEX wave = {
         .Format            = WAVE_RAW,
         .Channels          = channels,
         .Frequency         = Self->Frequency,
         .AvgBytesPerSecond = Self->BytesPerSecond,
         .BlockAlign        = int16_t(channels * (Self->BitsPerSample>>3)),
         .BitsPerSample     = int16_t(Self->BitsPerSample),
         .ExtraLength       = 0
      };

      int buffer_len;
      if ((Self->Stream IS STREAM::ALWAYS) and (Self->Length > 16 * 1024)) {
         buffer_len = Self->BytesPerSecond * SECONDS_STREAM_BUFFER;
      }
      else if ((Self->Stream IS STREAM::SMART) and (Self->Length > 256 * 1024)) {
         buffer_len = Self->BytesPerSecond * SECONDS_STREAM_BUFFER;
      }
      else buffer_len = Self->Length;

      if (buffer_len > Self->Length) buffer_len = Self->Length;

      CSTRING strerr;
      if (Self->Length > buffer_len) {
         log.msg("Streaming enabled because sample length %d exceeds buffer size %d.", Self->Length, buffer_len);
         Self->Flags |= SDF::STREAM;
         strerr = sndCreateBuffer(Self, &wave, buffer_len, Self->Length, (PlatformData *)Self->PlatformData, true);
      }
      else {
         // Create the buffer and fill it completely with sample data.
         buffer_len = Self->Length;
         Self->Flags &= ~SDF::STREAM;
         auto client_pos = Self->Position; // Save the seek cursor from pollution
         if (client_pos) Self->seekStart(0);
         strerr = sndCreateBuffer(Self, &wave, buffer_len, Self->Length, (PlatformData *)Self->PlatformData, false);
         Self->seekStart(client_pos);
      }

      if (strerr) {
         log.warning("Failed to create audio buffer, reason: %s (sample length %d)", strerr, Self->Length);
         return ERR::Failed;
      }

      Self->Active = true;
   }

   if (Self->AudioID) {
      pf::ScopedObjectLock<extAudio> audio(Self->AudioID, 200);
      if (audio.granted()) {
         sndVolume((PlatformData *)Self->PlatformData, audio->MasterVolume * Self->Volume);
      }
   }
   else sndVolume((PlatformData *)Self->PlatformData, Self->Volume);

   sndFrequency((PlatformData *)Self->PlatformData, Self->Playback);
   sndPan((PlatformData *)Self->PlatformData, Self->Pan);

   if ((Self->Flags & SDF::STREAM) != SDF::NIL) {
      if (SubscribeTimer(0.25, C_FUNCTION(win32_audio_stream), &Self->StreamTimer) != ERR::Okay) return log.warning(ERR::Failed);
   }
   else if (set_playback_trigger(Self) != ERR::Okay) return log.warning(ERR::Failed);

   auto response = sndPlay((PlatformData *)Self->PlatformData, ((Self->Flags & SDF::LOOP) != SDF::NIL) ? true : false, Self->Position);
   return response ? log.warning(ERR::Failed) : ERR::Okay;
#else

   if (!Self->Active) {
      // Determine the sample type

      auto sampleformat = SFM::NIL;
      if (Self->BitsPerSample IS 8) {
         if ((Self->Flags & SDF::STEREO) != SDF::NIL) sampleformat = SFM::U8_BIT_STEREO;
         else sampleformat = SFM::U8_BIT_MONO;
      }
      else if (Self->BitsPerSample IS 16) {
         if ((Self->Flags & SDF::STEREO) != SDF::NIL) sampleformat = SFM::S16_BIT_STEREO;
         else sampleformat = SFM::S16_BIT_MONO;
      }

      if (sampleformat IS SFM::NIL) return log.warning(ERR::InvalidData);

      // Create the audio buffer and fill it with sample data

      if ((Self->Stream IS STREAM::ALWAYS) and (Self->Length > 16 * 1024)) Self->Flags |= SDF::STREAM;
      else if ((Self->Stream IS STREAM::SMART) and (Self->Length > 256 * 1024)) Self->Flags |= SDF::STREAM;

      int8_t *buffer;
      if ((Self->Flags & SDF::STREAM) != SDF::NIL) {
         log.msg("Streaming enabled for playback in format $%.8x; Length: %d", int(sampleformat), Self->Length);

         struct snd::AddStream stream;
         AudioLoop loop;
         if ((Self->Flags & SDF::LOOP) != SDF::NIL) {
            loop.LoopMode   = LOOP::SINGLE;
            loop.Loop1Type  = LTYPE::UNIDIRECTIONAL;
            loop.Loop1Start = Self->LoopStart;
            if (Self->LoopEnd) loop.Loop1End = Self->LoopEnd;
            else loop.Loop1End = Self->Length;

            stream.Loop     = &loop;
            stream.LoopSize = sizeof(loop);
         }
         else {
            stream.Loop     = nullptr;
            stream.LoopSize = 0;
         }

         if (Self->OnStop.defined()) stream.OnStop = C_FUNCTION(onstop_event);
         else stream.OnStop.clear();

         stream.PlayOffset   = Self->Position;
         stream.Callback     = C_FUNCTION(read_stream);
         stream.SampleFormat = sampleformat;
         stream.SampleLength = Self->Length;

         pf::ScopedObjectLock<extAudio> audio(Self->AudioID, 250);
         if (audio.granted()) {
            if (Action(snd::AddStream::id, *audio, &stream) IS ERR::Okay) {
               Self->Handle = stream.Result;
            }
            else {
               log.warning("Failed to add sample to the Audio device.");
               return ERR::Failed;
            }
         }
         else return ERR::AccessObject;
      }
      else if (AllocMemory(Self->Length, MEM::DATA|MEM::NO_CLEAR, &buffer) IS ERR::Okay) {
         auto dc = deferred_call([&buffer] { FreeResource(buffer); });

         auto client_pos = Self->Position;
         if (Self->Position) Self->seekStart(0); // Ensure we're reading the entire sample from the start

         int result;
         if (Self->read(buffer, Self->Length, &result) IS ERR::Okay) {
            if (result != Self->Length) log.warning("Expected %d bytes, read %d", Self->Length, result);

            Self->seekStart(client_pos);

            struct snd::AddSample add;
            AudioLoop loop;

            if ((Self->Flags & SDF::LOOP) != SDF::NIL) {
               loop.LoopMode   = LOOP::SINGLE;
               loop.Loop1Type  = LTYPE::UNIDIRECTIONAL;
               loop.Loop1Start = Self->LoopStart;
               if (Self->LoopEnd) loop.Loop1End = Self->LoopEnd;
               else loop.Loop1End = Self->Length;

               add.Loop     = &loop;
               add.LoopSize = sizeof(loop);
            }
            else {
               add.Loop     = nullptr;
               add.LoopSize = 0;
            }

            if (Self->OnStop.defined()) add.OnStop = C_FUNCTION(onstop_event);
            else add.OnStop.clear();

            add.SampleFormat = sampleformat;
            add.Data         = buffer;
            add.DataSize     = Self->Length;

            pf::ScopedObjectLock<extAudio> audio(Self->AudioID, 250);
            if (audio.granted()) {
               if (Action(snd::AddSample::id, *audio, &add) IS ERR::Okay) {
                  Self->Handle = add.Result;
               }
               else {
                  log.warning("Failed to add sample to the Audio device.");
                  return ERR::Failed;
               }
            }
            else return log.warning(ERR::AccessObject);
         }
         else return log.warning(ERR::Read);
      }
      else return log.warning(ERR::AllocMemory);
   }

   Self->Active = true;

   pf::ScopedObjectLock<extAudio> audio(Self->AudioID, 2000);
   if (audio.granted()) {
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
            if (auto channel = audio->GetChannel(Self->ChannelIndex)) {
               if (channel->isStopped()) break;
               else if (channel->Priority < Self->Priority) priority = channel;
            }
            Self->ChannelIndex++;
         }

         if (i >= audio->MaxChannels) {
            if (!(channel = priority)) {
               log.msg("Audio channel not available for playback.");
               return ERR::Failed;
            }
         }
      }

      snd::MixStop(*audio, Self->ChannelIndex);

      if (snd::MixSample(*audio, Self->ChannelIndex, Self->Handle) IS ERR::Okay) {
         if (snd::MixVolume(*audio, Self->ChannelIndex, Self->Volume) != ERR::Okay) return log.warning(ERR::Failed);
         if (snd::MixPan(*audio, Self->ChannelIndex, Self->Pan) != ERR::Okay) return log.warning(ERR::Failed);
         if (snd::MixFrequency(*audio, Self->ChannelIndex, Self->Playback) != ERR::Okay) return log.warning(ERR::Failed);
         if (snd::MixPlay(*audio, Self->ChannelIndex, Self->Position) != ERR::Okay) return log.warning(ERR::Failed);

         return ERR::Okay;
      }
      else {
         log.warning("Failed to set sample %d to channel $%.8x", Self->Handle, Self->ChannelIndex);
         return ERR::Failed;
      }
   }
   else return log.warning(ERR::AccessObject);
#endif
}

//********************************************************************************************************************

static void notify_onstop_free(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   ((extSound *)CurrentContext())->OnStop.clear();
}

/*********************************************************************************************************************
-ACTION-
Deactivate: Stops the audio sample and resets the playback position.
-END-
*********************************************************************************************************************/

static ERR SOUND_Deactivate(extSound *Self)
{
   pf::Log log;

   log.branch();

   if (Self->StreamTimer) { UpdateTimer(Self->StreamTimer, 0); Self->StreamTimer = 0; }
   if (Self->PlaybackTimer) { UpdateTimer(Self->PlaybackTimer, 0); Self->PlaybackTimer = 0; }

   Self->Position = 0;

#ifdef USE_WIN32_PLAYBACK
   sndStop((PlatformData *)Self->PlatformData);
#else
   if (Self->ChannelIndex) {
      pf::ScopedObjectLock<extAudio> audio(Self->AudioID);
      if (audio.granted()) { // Stop the sample if it's live.
         if (auto channel = audio->GetChannel(Self->ChannelIndex)) {
            if (channel->SampleHandle IS Self->Handle) snd::MixStop(*audio, Self->ChannelIndex);
         }
      }
      else return log.warning(ERR::AccessObject);
   }
#endif

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Disable: Disable playback of an active audio sample, equivalent to pausing.
-END-
*********************************************************************************************************************/

static ERR SOUND_Disable(extSound *Self)
{
   pf::Log log;

   log.branch();

#ifdef USE_WIN32_PLAYBACK
   sndStop((PlatformData *)Self->PlatformData);
#else
   if (!Self->ChannelIndex) return ERR::Okay;

   pf::ScopedObjectLock<extAudio> audio(Self->AudioID, 5000);
   if (audio.granted()) {
      if (auto channel = audio->GetChannel(Self->ChannelIndex)) {
         if (channel->SampleHandle IS Self->Handle) snd::MixStop(*audio, Self->ChannelIndex);
      }
   }
   else return log.warning(ERR::AccessObject);
#endif

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Enable: Continues playing a sound if it has been disabled.
-END-
*********************************************************************************************************************/

static ERR SOUND_Enable(extSound *Self)
{
   pf::Log log;
   log.branch();

#ifdef USE_WIN32_PLAYBACK
   if (!Self->Handle) {
      log.msg("Playing back from position %" PF64, (long long)Self->Position);
      if ((Self->Flags & SDF::LOOP) != SDF::NIL) sndPlay((PlatformData *)Self->PlatformData, TRUE, Self->Position);
      else sndPlay((PlatformData *)Self->PlatformData, FALSE, Self->Position);
   }
#else
   if (!Self->ChannelIndex) return ERR::Okay;

   pf::ScopedObjectLock<extAudio> audio(Self->AudioID, 5000);
   if (audio.granted()) {
      if (auto channel = audio->GetChannel(Self->ChannelIndex)) {
         if (channel->SampleHandle IS Self->Handle) snd::MixContinue(*audio, Self->ChannelIndex);
      }
   }
   else return log.warning(ERR::AccessObject);
#endif

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR SOUND_Free(extSound *Self)
{
   if (Self->StreamTimer)   { UpdateTimer(Self->StreamTimer, 0); Self->StreamTimer = 0; }
   if (Self->PlaybackTimer) { UpdateTimer(Self->PlaybackTimer, 0); Self->PlaybackTimer = 0; }

   if (Self->OnStop.isScript()) {
      UnsubscribeAction(Self->OnStop.Context, AC::Free);
      Self->OnStop.clear();
   }

#if defined(USE_WIN32_PLAYBACK)
   if (!Self->Handle) sndFree((PlatformData *)Self->PlatformData);
#endif

   Self->deactivate();

   if ((Self->Handle) and (Self->AudioID)) {
      pf::ScopedObjectLock<extAudio> audio(Self->AudioID);
      if (audio.granted()) {
         audio->removeSample(Self->Handle);
         Self->Handle = 0;
      }
   }

   if (Self->Path) { FreeResource(Self->Path); Self->Path = nullptr; }
   if (Self->File) { FreeResource(Self->File); Self->File = nullptr; }

   Self->~extSound();
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
   if ((!Args) or (!Args->Key)) return ERR::NullArgs;

   std::string name(Args->Key);
   if (Self->Tags.contains(name)) {
      strcopy(Self->Tags[name], Args->Value, Args->Size);
      return ERR::Okay;
   }
   else return ERR::UnsupportedField;
}

/*********************************************************************************************************************
-ACTION-
Init: Prepares a sound object for usage.
-END-
*********************************************************************************************************************/

#ifdef USE_WIN32_PLAYBACK

static ERR SOUND_Init(extSound *Self)
{
   pf::Log log;
   int id, len;
   ERR error;

   // Find the local audio object or create one to ease the developer's workload.

   if (!Self->AudioID) {
      if ((error = snd_init_audio(Self)) != ERR::Okay) return error;
   }

   // Open channels for sound sample playback.

   if (!(Self->ChannelIndex = glSoundChannels[Self->AudioID])) {
      pf::ScopedObjectLock<extAudio> audio(Self->AudioID, 3000);
      if (audio.granted()) {
         if (audio->openChannels(audio->MaxChannels, &Self->ChannelIndex) IS ERR::Okay) {
            glSoundChannels[Self->AudioID] = Self->ChannelIndex;
         }
         else {
            log.warning("Failed to open audio channels.");
            return ERR::Failed;
         }
      }
      else return log.warning(ERR::AccessObject);
   }

   CSTRING path;
   if (((Self->Flags & SDF::NEW) != SDF::NIL) or (Self->get(FID_Path, path) != ERR::Okay) or (!path)) {
      // If the sample is new or no path has been specified, create an audio sample from scratch (e.g. to record
      // audio to disk).

      return ERR::Okay;
   }

   // Load the sound file's header and test it to see if it matches our supported file format.

   if (!Self->File) {
      if (!(Self->File = objFile::create::local(fl::Path(path), fl::Flags(FL::READ|FL::APPROXIMATE)))) {
         return log.warning(ERR::File);
      }
   }
   else Self->File->seekStart(0);

   Self->File->read(Self->Header, (int)sizeof(Self->Header));

   if ((std::string_view((char *)Self->Header, 4) != "RIFF") or
       (std::string_view((char *)Self->Header + 8, 4) != "WAVE")) {
      FreeResource(Self->File);
      Self->File = nullptr;
      return ERR::NoSupport;
   }

   // Read the RIFF header

   Self->File->seekStart(12);
   if (fl::ReadLE(Self->File, &id) != ERR::Okay) return ERR::Read; // Contains the characters "fmt "
   if (fl::ReadLE(Self->File, &len) != ERR::Okay) return ERR::Read; // Length of data in this chunk

   WAVEFormat WAVE;
   int result;
   if ((Self->File->read(&WAVE, len, &result) != ERR::Okay) or (result != len)) {
      return log.warning(ERR::Read);
   }

   // Check the format of the sound file's data

   if ((WAVE.Format != WAVE_ADPCM) and (WAVE.Format != WAVE_RAW)) {
      log.msg("This file's WAVE data format is not supported (type %d).", WAVE.Format);
      return ERR::InvalidData;
   }

   // Look for the "data" chunk

   if (find_chunk(Self, Self->File, "data") != ERR::Okay) {
      return log.warning(ERR::Read);
   }

   if (fl::ReadLE(Self->File, &Self->Length) != ERR::Okay) return ERR::Read; // Length of audio data in this chunk

   if (Self->Length & 1) Self->Length++;

   // Setup the sound structure

   Self->DataOffset = Self->File->get<int>(FID_Position);

   Self->Format         = WAVE.Format;
   Self->BytesPerSecond = WAVE.AvgBytesPerSecond;
   Self->BitsPerSample  = WAVE.BitsPerSample;
   if (WAVE.Channels IS 2) Self->Flags |= SDF::STEREO;
   if (Self->Frequency <= 0) Self->Frequency = WAVE.Frequency;
   if (Self->Playback <= 0)  Self->Playback  = Self->Frequency;

   if ((Self->Flags & SDF::NOTE) != SDF::NIL) {
      Self->set(FID_Note, Self->Note);
      Self->Flags &= ~SDF::NOTE;
   }

   log.trace("Bits: %d, Freq: %d, KBPS: %d, ByteLength: %d, DataOffset: %d", Self->BitsPerSample, Self->Frequency, Self->BytesPerSecond, Self->Length, Self->DataOffset);

   return ERR::Okay;
}

#else // Use the internal mixer

static ERR SOUND_Init(extSound *Self)
{
   pf::Log log;
   int id, len, result, pos;
   ERR error;

   if (!Self->AudioID) {
      if ((error = snd_init_audio(Self)) != ERR::Okay) return error;
   }

   // Open channels for sound sample playback.

   if (!(Self->ChannelIndex = glSoundChannels[Self->AudioID])) {
      pf::ScopedObjectLock<extAudio> audio(Self->AudioID, 3000);
      if (audio.granted()) {
         if (audio->openChannels(audio->MaxChannels, &Self->ChannelIndex) IS ERR::Okay) {
            glSoundChannels[Self->AudioID] = Self->ChannelIndex;
         }
         else {
            log.warning("Failed to open audio channels.");
            return ERR::Failed;
         }
      }
      else return log.warning(ERR::AccessObject);
   }

   auto path = Self->get<STRING>(FID_Path);

   if (((Self->Flags & SDF::NEW) != SDF::NIL) or (!path)) {
      log.msg("Sample created as new (without sample data).");

      // If the sample is new or no path has been specified, create an audio sample from scratch (e.g. to
      // record audio to disk).

      return ERR::Okay;
   }

   // Load the sound file's header and test it to see if it matches our supported file format.

   if (!Self->File) {
      if (!(Self->File = objFile::create::local(fl::Path(path), fl::Flags(FL::READ|FL::APPROXIMATE)))) {
         return log.warning(ERR::File);
      }
   }
   else Self->File->seekStart(0);

   Self->File->read(Self->Header, (int)sizeof(Self->Header));

   if ((std::string_view((char *)Self->Header, 4) != "RIFF") or
       (std::string_view((char *)Self->Header + 8, 4) != "WAVE")) {
      FreeResource(Self->File);
      Self->File = nullptr;
      return ERR::NoSupport;
   }

   // Read the FMT header

   Self->File->seek(12, SEEK::START);
   if (fl::ReadLE(Self->File, &id) != ERR::Okay) return ERR::Read; // Contains the characters "fmt "
   if (fl::ReadLE(Self->File, &len) != ERR::Okay) return ERR::Read; // Length of data in this chunk

   WAVEFormat WAVE;
   if ((Self->File->read(&WAVE, len, &result) != ERR::Okay) or (result < len)) {
      log.warning("Failed to read WAVE format header (got %d, expected %d)", result, len);
      return ERR::Read;
   }

   // Check the format of the sound file's data

   if ((WAVE.Format != WAVE_ADPCM) and (WAVE.Format != WAVE_RAW)) {
      log.warning("This file's WAVE data format is not supported (type %d).", WAVE.Format);
      return ERR::InvalidData;
   }

   // Look for the cue chunk for loop information

   pos = Self->File->get<int>(FID_Position);
#if 0
   if (find_chunk(Self, Self->File, "cue ") IS ERR::Okay) {
      data_p += 32;
      fl::ReadLE(Self->File, &info.loopstart);
      // if the next chunk is a LIST chunk, look for a cue length marker
      if (find_chunk(Self, Self->File, "LIST") IS ERR::Okay) {
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

   if (find_chunk(Self, Self->File, "data") != ERR::Okay) {
      return log.warning(ERR::Read);
   }

   // Setup the sound structure

   fl::ReadLE(Self->File, &Self->Length); // Length of audio data in this chunk

   Self->DataOffset = Self->File->get<int>(FID_Position);

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

#endif

//********************************************************************************************************************

static ERR SOUND_NewPlacement(extSound *Self)
{
   new (Self) extSound;
   Self->Compression = 50;     // 50% compression by default
   Self->Volume      = 1.0;    // Playback at 100% volume level
   Self->Pan         = 0;
   Self->Playback    = 0;
   Self->Note        = NOTE_C; // Standard pitch
   Self->Stream      = STREAM::SMART;
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
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.traceBranch("Length: %d, Offset: %" PF64, Args->Length, (long long)Self->Position);

   if (Args->Length <= 0) {
      Args->Result = 0;
      return ERR::Okay;
   }

   // Don't read more than the known raw sample length

   int result;
   if (Self->Position + Args->Length > Self->Length) {
      if (auto error = Self->File->read(Args->Buffer, Self->Length - Self->Position, &result); error != ERR::Okay) return error;
   }
   else if (auto error = Self->File->read(Args->Buffer, Args->Length, &result); error != ERR::Okay) return error;

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
   pf::Log log;

   // Divert this call if the developer is trying to save the sound data as a specific subclass type.

   if ((Args->ClassID != CLASSID::NIL) and (Args->ClassID != CLASSID::SOUND)) {
      auto mclass = (objMetaClass *)FindClass(Args->ClassID);

      ERR (**routine)(OBJECTPTR, APTR);
      if ((mclass->get(FID_ActionTable, routine) IS ERR::Okay) and (routine)) {
         if (routine[int(AC::SaveToObject)]) {
            return routine[int(AC::SaveToObject)](Self, Args);
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

   const int audio_data_size = Self->Length;

   header.DataChunkSize = audio_data_size;
   header.ChunkSize     = 36 + audio_data_size; // Header size (44) - 8 + data size

   if (acWrite(Args->Dest, &header, sizeof(header)) != ERR::Okay) return log.warning(ERR::Write);

   // Read and write audio data in chunks
   const int chunk_size = 8192;
   int bytes_remaining = audio_data_size;
   int original_position = Self->Position;

   if (Self->seekStart(0) != ERR::Okay) return log.warning(ERR::Seek);

   while (bytes_remaining > 0) {
      int read_size = (bytes_remaining < chunk_size) ? bytes_remaining : chunk_size;
      uint8_t buffer[8192];
      int bytes_read;
      if (acRead(Self, buffer, read_size, &bytes_read) != ERR::Okay) return log.warning(ERR::Read);
      if (acWrite(Args->Dest, buffer, bytes_read) != ERR::Okay) return log.warning(ERR::Write);
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
   pf::Log log;

   // NB: Sub-classes may divert their functionality to this routine if the sample is fully buffered.

   if (!Args) return log.warning(ERR::NullArgs);
   if (!Self->initialised()) return log.warning(ERR::NotInitialised);

   if (Args->Position IS SEEK::START)         Self->Position = int(Args->Offset);
   else if (Args->Position IS SEEK::END)      Self->Position = Self->Length - int(Args->Offset);
   else if (Args->Position IS SEEK::CURRENT)  Self->Position += int(Args->Offset);
   else if (Args->Position IS SEEK::RELATIVE) Self->Position = Self->Length * Args->Offset;
   else return log.warning(ERR::Args);

   if (Self->Position < 0) Self->Position = 0;
   else if (Self->Position > Self->Length) Self->Position = Self->Length;
   else { // Retain correct byte alignment.
      int align = ((((Self->Flags & SDF::STEREO) != SDF::NIL) ? 2 : 1) * (Self->BitsPerSample>>3)) - 1;
      Self->Position &= ~align;
   }

   log.traceBranch("Seek to %" PF64 " + %d", (long long)Self->Position, Self->DataOffset);

   pf::ScopedObjectLock<extAudio> audio(Self->AudioID, 2000);
   if (audio.granted()) {
      if ((Self->File) and (!Self->isSubClass())) {
         Self->File->seekStart(Self->DataOffset + Self->Position);
      }

      #ifdef USE_WIN32_PLAYBACK
         if (sndCheckActivity((PlatformData *)Self->PlatformData) > 0) {
            set_playback_trigger(Self);
            sndSetPosition((PlatformData *)Self->PlatformData, Self->Position);
         }
      #else
         if (Self->Handle) {
            audio->Samples[Self->Handle].PlayPos = BYTELEN(Self->Position);

            if ((!audio->Samples[Self->Handle].Stream) and (Self->Active)) {
               // Sample is fully buffered.  Adjust position now if it's in playback.

               if (Self->ChannelIndex) {
                  if (auto channel = audio->GetChannel(Self->ChannelIndex)) {
                     if (!channel->isStopped()) {
                        snd::MixPlay(*audio, Self->ChannelIndex, Self->Position);
                     }
                  }
               }
            }
         }
      #endif
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
   if ((!Args) or (!Args->Key) or (!Args->Key[0])) return ERR::NullArgs;

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
#ifdef USE_WIN32_PLAYBACK
   pf::Log log;

   if (Self->Active) {
      int16_t status = sndCheckActivity((PlatformData *)Self->PlatformData);

      if (status IS 0) *Value = FALSE;
      else if (status > 0) *Value = TRUE;
      else {
         log.warning("Error retrieving active status.");
         *Value = FALSE;
      }
   }
   else *Value = FALSE;

   return ERR::Okay;
#else
   *Value = FALSE;

   if (Self->ChannelIndex) {
      pf::ScopedObjectLock<extAudio> audio(Self->AudioID);
      if (audio.granted()) {
         if (auto channel = audio->GetChannel(Self->ChannelIndex)) {
            if (!channel->isStopped()) *Value = TRUE;
         }
      }
      else return ERR::AccessObject;
   }
#endif

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

static ERR SOUND_GET_Header(extSound *Self, int8_t **Value, int *Elements)
{
   *Value = (int8_t *)Self->Header;
   *Elements = std::ssize(Self->Header);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Length: Indicates the total byte-length of sample data.

This field specifies the length of the sample data in bytes.  To get the length of the sample in seconds, divide this
value by the #BytesPerSecond field.

*********************************************************************************************************************/

static ERR SOUND_SET_Length(extSound *Self, int Value)
{
   pf::Log log;
   if (Value >= 0) {
      Self->Length = Value;

      #ifdef USE_WIN32_PLAYBACK
         if (Self->initialised()) {
            sndLength((PlatformData *)Self->PlatformData, Value);
         }
         return ERR::Okay;
      #else
         if ((Self->Handle) and (Self->AudioID)) {
            pf::ScopedObjectLock<objAudio> audio(Self->AudioID);
            if (audio.granted()) {
               return audio->setSampleLength(Self->Handle, Value);
            }
            else return log.warning(ERR::AccessObject);
         }
         else return ERR::Okay;
      #endif
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

static ERR SOUND_GET_Note(extSound *Self, CSTRING *Value)
{
   switch(Self->Note) {
      case NOTE_C:  Self->NoteString[0] = 'C';
                    Self->NoteString[1] = '5' + Self->Octave;
                    Self->NoteString[2] = 0;
                    break;
      case NOTE_CS: Self->NoteString[0] = 'C';
                    Self->NoteString[1] = '5' + Self->Octave;
                    Self->NoteString[2] = '#';
                    Self->NoteString[3] = 0;
                    break;
      case NOTE_D:  Self->NoteString[0] = 'D';
                    Self->NoteString[1] = '5' + Self->Octave;
                    Self->NoteString[2] = 0;
                    break;
      case NOTE_DS: Self->NoteString[0] = 'D';
                    Self->NoteString[1] = '5' + Self->Octave;
                    Self->NoteString[2] = '#';
                    Self->NoteString[3] = 0;
                    break;
      case NOTE_E:  Self->NoteString[0] = 'E';
                    Self->NoteString[1] = '5' + Self->Octave;
                    Self->NoteString[2] = 0;
                    break;
      case NOTE_F:  Self->NoteString[0] = 'F';
                    Self->NoteString[1] = '5' + Self->Octave;
                    Self->NoteString[2] = 0;
                    break;
      case NOTE_FS: Self->NoteString[0] = 'F';
                    Self->NoteString[1] = '5' + Self->Octave;
                    Self->NoteString[2] = '#';
                    Self->NoteString[3] = 0;
                    break;
      case NOTE_G:  Self->NoteString[0] = 'G';
                    Self->NoteString[1] = '5' + Self->Octave;
                    Self->NoteString[2] = 0;
                    break;
      case NOTE_GS: Self->NoteString[0] = 'G';
                    Self->NoteString[1] = '5' + Self->Octave;
                    Self->NoteString[2] = '#';
                    Self->NoteString[3] = 0;
                    break;
      case NOTE_A:  Self->NoteString[0] = 'A';
                    Self->NoteString[1] = '5' + Self->Octave;
                    Self->NoteString[2] = 0;
                    break;
      case NOTE_AS: Self->NoteString[0] = 'A';
                    Self->NoteString[1] = '5' + Self->Octave;
                    Self->NoteString[2] = '#';
                    Self->NoteString[3] = 0;
                    break;
      case NOTE_B:  Self->NoteString[0] = 'B';
                    Self->NoteString[1] = '5' + Self->Octave;
                    Self->NoteString[2] = 0;
                    break;
      default:      Self->NoteString[0] = 0;
   }
   *Value = Self->NoteString;

   return ERR::Okay;
}

static ERR SOUND_SET_Note(extSound *Self, CSTRING Value)
{
   pf::Log log;

   if (!*Value) return ERR::Okay;

   int i, note;
   for (i=0; (Value[i]) and (i < 3); i++) Self->NoteString[i] = Value[i];
   Self->NoteString[i] = 0;

   CSTRING str = Value;
   if (((*Value >= '0') and (*Value <= '9')) or (*Value IS '-')) {
      note = strtol(Value, nullptr, 0);
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

#ifdef USE_WIN32_PLAYBACK
   if (Self->initialised()) {
      sndFrequency((PlatformData *)Self->PlatformData, Self->Playback);
   }
#else
   if (Self->ChannelIndex) {
      pf::ScopedObjectLock<extAudio> audio(Self->AudioID, 200);
      if (audio.granted()) {
         snd::MixFrequency(*audio, Self->ChannelIndex, Self->Playback);
      }
      else return ERR::AccessObject;
   }
#endif

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
   return Self->set(FID_Note, Self->Note);
}

/*********************************************************************************************************************

-FIELD-
OnStop: This callback is triggered when sample playback stops.

Set OnStop to a callback function to receive an event trigger when sample playback stops.  The prototype for the
function is `void OnStop(*Sound)`.

The timing of this event does not guarantee precision, but should be accurate to approximately 1/100th of a second
in most cases.

*********************************************************************************************************************/

static ERR SOUND_GET_OnStop(extSound *Self, FUNCTION **Value)
{
   if (Self->OnStop.defined()) {
      *Value = &Self->OnStop;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SOUND_SET_OnStop(extSound *Self, FUNCTION *Value)
{
   if (Value) {
      if (Self->OnStop.isScript()) UnsubscribeAction(Self->OnStop.Context, AC::Free);
      Self->OnStop = *Value;
      if (Self->OnStop.isScript()) {
         SubscribeAction(Self->OnStop.Context, AC::Free, C_FUNCTION(notify_onstop_free));
      }
   }
   else Self->OnStop.clear();
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

#ifdef USE_WIN32_PLAYBACK
   if (Self->initialised()) {
      sndPan((PlatformData *)Self->PlatformData, Self->Pan);
   }
#else
   if (Self->ChannelIndex) {
      pf::ScopedObjectLock<extAudio> audio(Self->AudioID, 200);
      if (audio.granted()) {
         snd::MixPan(*audio, Self->ChannelIndex, Self->Pan);
      }
      else return ERR::AccessObject;
   }
#endif

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Path: Location of the audio sample data.

This field must refer to a file that contains the audio data that will be loaded.  If creating a new sample
with the `SDF::NEW` flag, it is not necessary to define a file source.

*********************************************************************************************************************/

static ERR SOUND_GET_Path(extSound *Self, STRING *Value)
{
   if ((*Value = Self->Path)) return ERR::Okay;
   else return ERR::FieldNotSet;
}

static ERR SOUND_SET_Path(extSound *Self, CSTRING Value)
{
   pf::Log log;

   if (Self->Path) { FreeResource(Self->Path); Self->Path = nullptr; }

   if ((Value) and (*Value)) {
      int i = strlen(Value);
      if (AllocMemory(i+1, MEM::STRING|MEM::NO_CLEAR, (void **)&Self->Path) IS ERR::Okay) {
         for (i=0; Value[i]; i++) Self->Path[i] = Value[i];
         Self->Path[i] = 0;
      }
      else return log.warning(ERR::AllocMemory);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Playback: The playback frequency of the sound sample can be defined here.

Set this field to define the exact frequency of a sample's playback.  The playback frequency can be modified at
any time, including during audio playback if real-time adjustments to a sample's audio output rate is desired.

*********************************************************************************************************************/

static ERR SOUND_SET_Playback(extSound *Self, int Value)
{
   pf::Log log;

   if ((Value < 0) or (Value > 500000)) return ERR::OutOfRange;

   Self->Playback = Value;
   Self->Flags &= ~SDF::NOTE;

#ifdef USE_WIN32_PLAYBACK
   if (Self->initialised()) {
      sndFrequency((PlatformData *)Self->PlatformData, Self->Playback);
      if (Self->PlaybackTimer) set_playback_trigger(Self);
   }
#else
   if (Self->ChannelIndex) {
      pf::ScopedObjectLock<extAudio> audio(Self->AudioID, 200);
      if (audio.granted()) {
         snd::MixFrequency(*audio, Self->ChannelIndex, Self->Playback);
      }
      else return log.warning(ERR::AccessObject);
   }
#endif

   return ERR::Okay;
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

#ifdef USE_WIN32_PLAYBACK
   if (Self->initialised()) {
      pf::ScopedObjectLock<extAudio> audio(Self->AudioID, 200);
      if (audio.granted()) {
         sndVolume((PlatformData *)Self->PlatformData, audio->MasterVolume * Self->Volume);
      }
   }
#else
   if (Self->ChannelIndex) {
      pf::ScopedObjectLock<extAudio> audio(Self->AudioID, 200);
      if (audio.granted()) {
         snd::MixVolume(*audio, Self->ChannelIndex, Self->Volume);
      }
      else return ERR::AccessObject;
   }
#endif

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR find_chunk(extSound *Self, objFile *File, std::string_view ChunkName)
{
   while (true) {
      char chunk[4];
      int len;
      if ((File->read(chunk, sizeof(chunk), &len) != ERR::Okay) or (len != sizeof(chunk))) {
         return ERR::Read;
      }

      if (ChunkName IS std::string_view(chunk, 4)) return ERR::Okay;

      fl::ReadLE(Self->File, &len); // Length of data in this chunk
      Self->File->seekCurrent(len);
   }
}

//********************************************************************************************************************

#ifdef USE_WIN32_PLAYBACK
static ERR win32_audio_stream(extSound *Self, int64_t Elapsed, int64_t CurrentTime)
{
   pf::Log log(__FUNCTION__);

   // See snd::StreamAudio() for further information on streaming in Win32

   auto response = sndStreamAudio((PlatformData *)Self->PlatformData);
   if (response IS -1) {
      log.warning("Sound streaming failed.");
      sound_stopped_event(Self);
      if (Self->PlaybackTimer) { UpdateTimer(Self->PlaybackTimer, 0); Self->PlaybackTimer = 0; }
      Self->StreamTimer = 0;
      return ERR::Terminate;
   }
   else if (response IS 1) {
      Self->StreamTimer = 0;
      return ERR::Terminate;
   }

   return ERR::Okay;
}
#endif

//********************************************************************************************************************

static const FieldDef clFlags[] = {
   { "Loop",         (int)SDF::LOOP },
   { "New",          (int)SDF::NEW },
   { "Stereo",       (int)SDF::STEREO },
   { "RestrictPlay", (int)SDF::RESTRICT_PLAY },
   { nullptr, 0 }
};

static const FieldDef clStream[] = {
   { "Always", (int)STREAM::ALWAYS },
   { "Smart",  (int)STREAM::SMART },
   { "Never",  (int)STREAM::NEVER },
   { nullptr, 0 }
};

static const FieldArray clFields[] = {
   { "Volume",         FDF_DOUBLE|FDF_RW, nullptr, SOUND_SET_Volume },
   { "Pan",            FDF_DOUBLE|FDF_RW, nullptr, SOUND_SET_Pan },
   { "Position",       FDF_INT64|FDF_RW, nullptr, SOUND_SET_Position },
   { "Priority",       FDF_INT|FDF_RW, nullptr, SOUND_SET_Priority },
   { "Length",         FDF_INT|FDF_RW, nullptr, SOUND_SET_Length },
   { "Octave",         FDF_INT|FDF_RW, nullptr, SOUND_SET_Octave },
   { "Flags",          FDF_INTFLAGS|FDF_RW, nullptr, SOUND_SET_Flags, &clFlags },
   { "Frequency",      FDF_INT|FDF_RI },
   { "Playback",       FDF_INT|FDF_RW, nullptr, SOUND_SET_Playback },
   { "Compression",    FDF_INT|FDF_RW },
   { "BytesPerSecond", FDF_INT|FDF_RW },
   { "BitsPerSample",  FDF_INT|FDF_RW },
   { "Audio",          FDF_OBJECTID|FDF_RI },
   { "LoopStart",      FDF_INT|FDF_RW },
   { "LoopEnd",        FDF_INT|FDF_RW },
   { "Stream",         FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clStream },
   { "Handle",         FDF_INT|FDF_SYSTEM|FDF_R },
   { "ChannelIndex",   FDF_INT|FDF_R },
   // Virtual fields
   { "Active",   FDF_INT|FDF_R,           SOUND_GET_Active },
   { "Duration", FDF_DOUBLE|FDF_R,         SOUND_GET_Duration },
   { "Header",   FDF_BYTE|FDF_ARRAY|FDF_R, SOUND_GET_Header },
   { "OnStop",   FDF_FUNCTIONPTR|FDF_RW,   SOUND_GET_OnStop, SOUND_SET_OnStop },
   { "Path",     FDF_STRING|FDF_RI,        SOUND_GET_Path, SOUND_SET_Path },
   { "Src",      FDF_SYNONYM|FDF_STRING|FDF_RI, SOUND_GET_Path, SOUND_SET_Path },
   { "Note",     FDF_STRING|FDF_RW, SOUND_GET_Note, SOUND_SET_Note },
   END_FIELD
};

static const ActionArray clActions[] = {
   { AC::Activate,      SOUND_Activate },
   { AC::Deactivate,    SOUND_Deactivate },
   { AC::Disable,       SOUND_Disable },
   { AC::Enable,        SOUND_Enable },
   { AC::Free,          SOUND_Free },
   { AC::GetKey,        SOUND_GetKey },
   { AC::Init,          SOUND_Init },
   { AC::NewPlacement,  SOUND_NewPlacement },
   { AC::Read,          SOUND_Read },
   { AC::SaveToObject,  SOUND_SaveToObject },
   { AC::Seek,          SOUND_Seek },
   { AC::SetKey,        SOUND_SetKey },
   { AC::NIL, nullptr }
};

//********************************************************************************************************************

ERR add_sound_class(void)
{
   clSound = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::SOUND),
      fl::ClassVersion(VER_SOUND),
      fl::FileExtension("*.wav|*.wave|*.snd"),
      fl::FileDescription("Sound Sample"),
      fl::FileHeader("[0:$52494646][8:$57415645]"),
      fl::Icon("filetypes/audio"),
      fl::Name("Sound"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clActions),
      fl::Fields(clFields),
      fl::Size(sizeof(extSound)),
      fl::Path(MOD_PATH));

   return clSound ? ERR::Okay : ERR::AddClass;
}

void free_sound_class(void)
{
   if (clSound) { FreeResource(clSound); clSound = 0; }
}
