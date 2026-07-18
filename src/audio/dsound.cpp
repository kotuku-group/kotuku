
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>
#include <math.h>
#include <kotuku/system/errors_c.h>

#define IS  ==

#define FILL_FIRST  1
#define FILL_SECOND 2

class Object;
class extAudio;

struct PlatformData {
   LPDIRECTSOUNDBUFFER SoundBuffer;
   Object *Object;
   DWORD  BufferLength;
   DWORD  Position;      // Total number of bytes that have so far been loaded from the audio data source
   DWORD  SampleLength;  // Total length of the original sample
   DWORD  BufferPos;
   char   Fill;
   char   Stream;
   char   Stop;
   bool   Loop;
};

extern "C" int dsReadData(Object *, void *, int);
extern "C" void dsSeekData(Object *, int);

#include "windows.h"

static LPDIRECTSOUND glDirectSound;     // the DirectSound object
static HMODULE dsModule = nullptr;         // dsound.dll module handle
static HWND glWindow;                   // HWND for DirectSound

static HRESULT (WINAPI *dsDirectSoundCreate)(const GUID *, LPDIRECTSOUND *, IUnknown FAR *) = nullptr;

//********************************************************************************************************************
// DirectSound uses logarithmic values for volume.  If there's a need to optimise this, generate a lookup table.

inline int linear2ds(float Volume)
{
   if (Volume <= 0.01) return DSBVOLUME_MIN;
   else return (int)floor(2000.0 * log10(Volume) + 0.5);
}

//********************************************************************************************************************

extern "C" const char * dsInitDevice(int mixRate)
{
   glWindow = GetDesktopWindow();

   if (!glWindow) return "Failed to get desktop window.";

   // If the DirectSound DLL hasn't been loaded yet, load it and get the address for DirectSoundCreate.

   if ((dsModule IS nullptr) or (dsDirectSoundCreate IS nullptr) ) {
      if ((dsModule = LoadLibrary("dsound.dll")) IS nullptr) {
         return "Couldn't load dsound.dll";
      }

      if ((dsDirectSoundCreate = (HRESULT (*)(const GUID*, IDirectSound**, IUnknown*))GetProcAddress(dsModule, "DirectSoundCreate")) IS nullptr) {
         return "Couldn't get DirectSoundCreate address";
      }
   }

   if (dsDirectSoundCreate(nullptr, &glDirectSound, nullptr) != DS_OK) return "Failed in call to DirectSoundCreate().";

   if (glDirectSound->SetCooperativeLevel(glWindow, DSSCL_PRIORITY) != DS_OK)
      return "Failed in call to SetCooperativeLevel().";

   return nullptr;
}

//********************************************************************************************************************

extern "C" void dsCloseDevice(void)
{
   if (!glDirectSound) return;

   glDirectSound->Release();
   glDirectSound = 0;
}

//********************************************************************************************************************

int sndCheckActivity(PlatformData *Sound)
{
   DWORD status;

   if (!glDirectSound) return -1;
   if (!Sound->SoundBuffer) return -1; // Error

   if (IDirectSoundBuffer_GetStatus(Sound->SoundBuffer, &status) IS DS_OK) {
      if (status & DSBSTATUS_PLAYING) {
         return 1;
      }
      else return 0;
   }
   else return -1; // Error
}

//********************************************************************************************************************

int sndGetPosition(PlatformData *Sound, int64_t *Position)
{
   if ((!Position) or (!glDirectSound) or (!Sound->SoundBuffer)) return -1;

   DWORD play_pos;
   if (IDirectSoundBuffer_GetCurrentPosition(Sound->SoundBuffer, &play_pos, nullptr) != DS_OK) return -1;

   if (!Sound->Stream) {
      *Position = play_pos;
      return 0;
   }

   int64_t unread;
   if (Sound->Fill IS FILL_SECOND) {
      const auto half = Sound->BufferLength>>1;
      if (play_pos < half) unread = int64_t(half - play_pos);
      else unread = int64_t(Sound->BufferLength - play_pos) + int64_t(half);
   }
   else unread = int64_t(Sound->BufferLength - play_pos);

   int64_t pos = int64_t(Sound->Position) - unread;

   if ((Sound->Loop) and (Sound->SampleLength > 0)) {
      while (pos < 0) pos += Sound->SampleLength;
      pos = pos % Sound->SampleLength;
   }
   else {
      if (pos < 0) pos = 0;
      else if (pos > Sound->SampleLength) pos = Sound->SampleLength;
   }

   *Position = pos;
   return 0;
}

static const GUID pa_KSDATAFORMAT_SUBTYPE_WAVEFORMATEX = { STATIC_KSDATAFORMAT_SUBTYPE_WAVEFORMATEX }; // Standard PCM
static const GUID pa_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = { STATIC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT }; // 32-bit floats

//********************************************************************************************************************
// SampleLength: The byte length of the raw audio data, excludes all file headers.

extern "C" const char * sndCreateBuffer(Object *Object, void *Wave, int BufferLength, int SampleLength, PlatformData *Sound, int Stream)
{
   if (!glDirectSound) return 0;

   if (Sound->SoundBuffer) { // Release any buffer from a previous activation to prevent a resource leak
      IDirectSoundBuffer_Stop(Sound->SoundBuffer);
      IDirectSoundBuffer_Release(Sound->SoundBuffer);
      Sound->SoundBuffer = nullptr;
   }

   Sound->Object       = Object;
   Sound->SampleLength = SampleLength;
   Sound->BufferLength = BufferLength;
   Sound->Position     = 0;
   Sound->Stream       = Stream;
   Sound->Fill         = FILL_FIRST; // First half waiting to be filled

   DSBUFFERDESC dsbdesc;
   ZeroMemory(&dsbdesc, sizeof(DSBUFFERDESC));
   dsbdesc.dwSize  = sizeof(DSBUFFERDESC);
   dsbdesc.dwFlags = DSBCAPS_GETCURRENTPOSITION2
                     |DSBCAPS_GLOBALFOCUS
                     |DSBCAPS_CTRLVOLUME
                     |DSBCAPS_CTRLPAN
                     |DSBCAPS_CTRLFREQUENCY
                     |DSBCAPS_CTRLPOSITIONNOTIFY;
   dsbdesc.dwBufferBytes = BufferLength;
   dsbdesc.lpwfxFormat   = (LPWAVEFORMATEX)Wave;
   if ((IDirectSound_CreateSoundBuffer(glDirectSound, &dsbdesc, &Sound->SoundBuffer, nullptr) != DS_OK) or (!Sound->SoundBuffer)) {
      return "CreateSoundBuffer() failed to create WAVE audio buffer.";
   }

   if (Stream) return nullptr;

   // Fill the buffer with audio content completely if it's not streamed.

   void *bufA, *bufB;
   int lenA, lenB;
   if (IDirectSoundBuffer_Lock(Sound->SoundBuffer, 0, BufferLength, (void **)&bufA, (DWORD *)&lenA, (void **)&bufB, (DWORD *)&lenB, 0) IS DS_OK) {
      if (lenA) lenA = dsReadData(Object, bufA, lenA);
      if (lenB) lenB = dsReadData(Object, bufB, lenB);
      IDirectSoundBuffer_Unlock(Sound->SoundBuffer, bufA, lenA, bufB, lenB); // lenA/B inform DSound as to how many bytes were written.
   }

   return nullptr;
}

//********************************************************************************************************************

void sndFree(PlatformData *Info)
{
   if (!glDirectSound) return;

   if (Info->SoundBuffer) {
      IDirectSoundBuffer_Stop(Info->SoundBuffer);     // Stop the playback buffer
      IDirectSoundBuffer_Release(Info->SoundBuffer);  // Free the playback buffer
      Info->SoundBuffer = nullptr;
   }
}

//********************************************************************************************************************

void sndFrequency(PlatformData *Sound, int Frequency)
{
   if (!glDirectSound) return;
   if (Sound->SoundBuffer) IDirectSoundBuffer_SetFrequency(Sound->SoundBuffer, Frequency);
}

//********************************************************************************************************************

void sndPan(PlatformData *Sound, float Pan)
{
   if (!glDirectSound) return;

   // Range -10,000 to 10,000 (DSBPAN_LEFT to DSBPAN_RIGHT)
   int pan = int(Pan * DSBPAN_RIGHT);
   if (pan < DSBPAN_LEFT) pan = DSBPAN_LEFT;
   else if (pan > DSBPAN_RIGHT) pan = DSBPAN_RIGHT;
   if (Sound->SoundBuffer) IDirectSoundBuffer_SetPan(Sound->SoundBuffer, pan);
}

//********************************************************************************************************************

void sndStop(PlatformData *Sound)
{
   if (!glDirectSound) return;
   if (Sound->SoundBuffer) IDirectSoundBuffer_Stop(Sound->SoundBuffer);
}

//********************************************************************************************************************
// Used by the Sound class to play WAV or raw audio samples that are independent of our custom mixer.

__declspec(no_sanitize_address) int sndPlay(PlatformData *Sound, bool Loop, int Offset)
{
#ifdef __SANITIZE_ADDRESS__
   // There is an issue with the address sanitizer being tripped in calls to DirectSound under no client fault.
   // The no_sanitize_address option doesn't seem to work as expected, so for the time being DirectSound
   // is disabled if the sanitizer is enabled.
   return ERR_NoSupport;
#else
   if ((!Sound) or (!Sound->SoundBuffer)) return ERR_Args;

   if (Offset < 0) Offset = 0;
   else if (Offset >= (int)Sound->SampleLength) return ERR_OutOfRange;

   Sound->Loop = Loop;

   if (Sound->Stream) {
      // Streamed samples require that we reload sound data from scratch.  This call initiates the streaming playback,
      // after which sndStreamAudio() needs to be used to keep filling the buffer.

      IDirectSoundBuffer_Stop(Sound->SoundBuffer);

      Sound->Fill = FILL_FIRST;
      Sound->Stop = 0;

      unsigned char *bufA, *bufB;
      int lenA, lenB;
      if (IDirectSoundBuffer_Lock(Sound->SoundBuffer, 0, Sound->BufferLength, (void **)&bufA, (DWORD *)&lenA, (void **)&bufB, (DWORD *)&lenB, 0) IS DS_OK) {
         dsSeekData(Sound->Object, Offset);
         Sound->Position = Offset;
         if (lenA > 0) {
            auto lenA2 = dsReadData(Sound->Object, bufA, lenA);
            if (lenA2 < lenA) ZeroMemory(bufA + lenA2, lenA - lenA2);
            Sound->Position += lenA2;
         }
         IDirectSoundBuffer_Unlock(Sound->SoundBuffer, bufA, lenA, bufB, 0);
      }
      else return ERR_LockFailed;
   }
   else { // For non-streamed samples, start the play position from the proposed offset
      IDirectSoundBuffer_Stop(Sound->SoundBuffer);
      IDirectSoundBuffer_SetCurrentPosition(Sound->SoundBuffer, Offset);
   }

   // Play the sound

   if ((Sound->Loop) or (Sound->Stream)) {
      IDirectSoundBuffer_Play(Sound->SoundBuffer, 0, 0, DSBPLAY_LOOPING);
   }
   else IDirectSoundBuffer_Play(Sound->SoundBuffer, 0, 0, 0);

   return ERR_Okay;
#endif
}

//********************************************************************************************************************
// Streaming audio process for WAV or raw audio samples played via the Sound class.  This is regularly called by the
// Sound class' timer.

extern "C" int sndStreamAudio(PlatformData *Sound)
{
   unsigned char *write, *write2;

   if (!glDirectSound) return -1;
   if (!Sound->SoundBuffer) return -1;

   // Get the current play position.  NB: The BufferPos will cycle.

   if (IDirectSoundBuffer_GetCurrentPosition(Sound->SoundBuffer, &Sound->BufferPos, nullptr) IS DS_OK) {
      // If the playback marker is in the buffer's first half, we fill the second half, and vice versa.

      int lock_start = -1;
      int lock_length = 0;
      if ((Sound->Fill IS FILL_FIRST) and (Sound->BufferPos >= Sound->BufferLength>>1)) {
         Sound->Fill = FILL_SECOND;
         lock_start  = 0;
         lock_length = Sound->BufferLength>>1;
         if (Sound->Stop > 1) {
            IDirectSoundBuffer_Stop(Sound->SoundBuffer);
            return 1;
         }
      }
      else if ((Sound->Fill IS FILL_SECOND) and (Sound->BufferPos < Sound->BufferLength>>1)) {
         Sound->Fill = FILL_FIRST;
         lock_start  = Sound->BufferLength>>1;
         lock_length = Sound->BufferLength - (Sound->BufferLength>>1);
         if (Sound->Stop > 1) {
            IDirectSoundBuffer_Stop(Sound->SoundBuffer);
            return 1;
         }
      }

      if (lock_start != -1) {
         // Load more data if we have entered the next audio buffer section

         int length, length2, bytes_out;
         if (IDirectSoundBuffer_Lock(Sound->SoundBuffer, lock_start, lock_length, (void **)&write, (DWORD *)&length, (void **)&write2, (DWORD *)&length2, 0) IS DS_OK) {
            auto len = length;
            if (Sound->Position + len > Sound->SampleLength) len = Sound->SampleLength - Sound->Position;

            if (len > 0) {
               bytes_out = dsReadData(Sound->Object, write, len);
               Sound->Position += bytes_out;
            }
            else bytes_out = 0;

            if (Sound->Position >= Sound->SampleLength) {
               // All of the bytes have been read from the sample.

               if (Sound->Loop) {
                  dsSeekData(Sound->Object, 0);
                  bytes_out = dsReadData(Sound->Object, write + bytes_out, length - bytes_out);
                  Sound->Position = bytes_out;
               }
               else {
                  Sound->Stop++;
                  if (length - bytes_out > 0) ZeroMemory(write+bytes_out, length-bytes_out); // Clear trailing data for a clean exit
                  if (length2 > 0) ZeroMemory(write2, length2);

                  if (Sound->Stop IS 1) {
                     if (Sound->Fill IS FILL_FIRST) {
                        end_of_stream(Sound->Object, (Sound->BufferLength>>1) - Sound->BufferPos + bytes_out);
                     }
                     else end_of_stream(Sound->Object, (Sound->BufferLength - Sound->BufferPos) + bytes_out);
                  }
               }
            }

            IDirectSoundBuffer_Unlock(Sound->SoundBuffer, write, bytes_out, write2, 0);
         }
      }
      return 0;
   }
   else return -1;
}

//********************************************************************************************************************
// Adjusting the length is supported for streaming samples only.

void sndLength(PlatformData *Sound, int Length)
{
   if (!glDirectSound) return;

   Sound->SampleLength = Length;
}

//********************************************************************************************************************

void sndVolume(PlatformData *Sound, float Volume)
{
   if (!glDirectSound) return;

   if (Sound->SoundBuffer) {
      IDirectSoundBuffer_SetVolume(Sound->SoundBuffer, linear2ds(Volume));
   }
}

//********************************************************************************************************************
// Intended for calls from Sound.Seek() exclusively.

void sndSetPosition(PlatformData *Sound, int Offset)
{
   if (!glDirectSound) return;
   if (!Sound->SoundBuffer) return;

   if (Sound->Stream) { // Streams require resetting because the buffer will be stale.
      sndPlay(Sound, Sound->Loop, Offset);
   }
   else {
      IDirectSoundBuffer_SetCurrentPosition(Sound->SoundBuffer, Offset);
      Sound->Position = Offset;
   }
}

//********************************************************************************************************************
// Windows system beep using hardware speaker

extern "C" int sndBeep(int Pitch, int Duration)
{
   return Beep(Pitch, Duration) ? 1 : 0;
}
