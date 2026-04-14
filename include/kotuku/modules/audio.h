#pragma once

// Name:      audio.h
// Copyright: Paul Manias © 2002-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_AUDIO (1)

class objAudio;
class objSound;

// Optional flags for the Audio object.

enum class ADF : uint32_t {
   NIL = 0,
   OVER_SAMPLING = 0x00000001,
   FILTER_LOW = 0x00000002,
   FILTER_HIGH = 0x00000004,
   STEREO = 0x00000008,
   VOL_RAMPING = 0x00000010,
   AUTO_SAVE = 0x00000020,
   SYSTEM_WIDE = 0x00000040,
};

DEFINE_ENUM_FLAG_OPERATORS(ADF)

// Volume control flags

enum class VCF : uint32_t {
   NIL = 0,
   PLAYBACK = 0x00000001,
   CAPTURE = 0x00000010,
   JOINED = 0x00000100,
   MONO = 0x00001000,
   MUTE = 0x00010000,
   SYNC = 0x00100000,
};

DEFINE_ENUM_FLAG_OPERATORS(VCF)

// Optional flags for the AudioChannel structure.

enum class CHF : uint32_t {
   NIL = 0,
   MUTE = 0x00000001,
   BACKWARD = 0x00000002,
   VOL_RAMP = 0x00000004,
   CHANGED = 0x00000008,
};

DEFINE_ENUM_FLAG_OPERATORS(CHF)

// Flags for the SetVolume() method.

enum class SVF : uint32_t {
   NIL = 0,
   MUTE = 0x00000100,
   UNMUTE = 0x00001000,
   CAPTURE = 0x00010000,
};

DEFINE_ENUM_FLAG_OPERATORS(SVF)

// Sound flags

enum class SDF : uint32_t {
   NIL = 0,
   LOOP = 0x00000001,
   NEW = 0x00000002,
   STEREO = 0x00000004,
   RESTRICT_PLAY = 0x00000008,
   STREAM = 0x40000000,
   NOTE = 0x80000000,
};

DEFINE_ENUM_FLAG_OPERATORS(SDF)

// These audio bit formats are supported by AddSample and AddStream.

enum class SFM : uint32_t {
   NIL = 0,
   F_BIG_ENDIAN = 0x80000000,
   U8_BIT_MONO = 1,
   S16_BIT_MONO = 2,
   U8_BIT_STEREO = 3,
   S16_BIT_STEREO = 4,
   END = 5,
};

// Loop modes for the AudioLoop structure.

enum class LOOP : int16_t {
   NIL = 0,
   SINGLE = 1,
   SINGLE_RELEASE = 2,
   DOUBLE = 3,
   AMIGA_NONE = 4,
   AMIGA = 5,
};

// Loop types for the AudioLoop structure.

enum class LTYPE : int8_t {
   NIL = 0,
   UNIDIRECTIONAL = 1,
   BIDIRECTIONAL = 2,
};

// Streaming options

enum class STREAM : int {
   NIL = 0,
   NEVER = 1,
   SMART = 2,
   ALWAYS = 3,
};

// Definitions for the Note field.  An 'S' indicates a sharp note.

#define NOTE_C 0
#define NOTE_CS 1
#define NOTE_D 2
#define NOTE_DS 3
#define NOTE_E 4
#define NOTE_F 5
#define NOTE_FS 6
#define NOTE_G 7
#define NOTE_GS 8
#define NOTE_A 9
#define NOTE_AS 10
#define NOTE_B 11
#define NOTE_OCTAVE 12

// Channel status types for the AudioChannel structure.

enum class CHS : int8_t {
   NIL = 0,
   STOPPED = 0,
   FINISHED = 1,
   PLAYING = 2,
   RELEASED = 3,
   FADE_OUT = 4,
};

struct AudioLoop {
   LOOP  LoopMode;   // Loop mode (single, double)
   LTYPE Loop1Type;  // First loop type (unidirectional, bidirectional)
   LTYPE Loop2Type;  // Second loop type (unidirectional, bidirectional)
   int   Loop1Start; // Start of the first loop
   int   Loop1End;   // End of the first loop
   int   Loop2Start; // Start of the second loop
   int   Loop2End;   // End of the second loop
};

// Audio class definition

#define VER_AUDIO (1.000000)

// Audio methods

namespace snd {
struct OpenChannels { int Total; int Result; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CloseChannels { int Handle; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct AddSample { FUNCTION OnStop; SFM SampleFormat; APTR Data; int DataSize; struct AudioLoop * Loop; int LoopSize; int Result; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RemoveSample { int Handle; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetSampleLength { int Sample; int64_t Length; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct AddStream { FUNCTION Callback; FUNCTION OnStop; SFM SampleFormat; int SampleLength; int PlayOffset; struct AudioLoop * Loop; int LoopSize; int Result; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Beep { int Pitch; int Duration; int Volume; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetVolume { int Index; CSTRING Name; SVF Flags; int Channel; double Volume; static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objAudio : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::AUDIO;
   static constexpr CSTRING CLASS_NAME = "Audio";

   using create = pf::Create<objAudio>;

   int OutputRate;    // Determines the frequency to use for the output of audio data.
   int InputRate;     // Determines the frequency to use when recording audio data.
   int Quality;       // Determines the quality of the audio mixing.
   ADF Flags;         // Special audio flags can be set here.
   int BitDepth;      // The bit depth affects the overall quality of audio input and output.
   int Periods;       // Defines the number of periods that make up the internal audio buffer.
   int PeriodSize;    // Defines the byte size of each period allocated to the internal audio buffer.

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR deactivate() noexcept { return Action(AC::Deactivate, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR saveSettings() noexcept { return Action(AC::SaveSettings, this, nullptr); }
   inline ERR saveToObject(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveToObject args = { Dest, { ClassID } };
      return Action(AC::SaveToObject, this, &args);
   }
   inline ERR openChannels(int Total, int * Result) noexcept {
      struct snd::OpenChannels args = { Total, (int)0 };
      ERR error = Action(AC(-1), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }
   inline ERR closeChannels(int Handle) noexcept {
      struct snd::CloseChannels args = { Handle };
      return(Action(AC(-2), this, &args));
   }
   inline ERR addSample(FUNCTION OnStop, SFM SampleFormat, APTR Data, int DataSize, struct AudioLoop * Loop, int LoopSize, int * Result) noexcept {
      struct snd::AddSample args = { OnStop, SampleFormat, Data, DataSize, Loop, LoopSize, (int)0 };
      ERR error = Action(AC(-3), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }
   inline ERR removeSample(int Handle) noexcept {
      struct snd::RemoveSample args = { Handle };
      return(Action(AC(-4), this, &args));
   }
   inline ERR setSampleLength(int Sample, int64_t Length) noexcept {
      struct snd::SetSampleLength args = { Sample, Length };
      return(Action(AC(-5), this, &args));
   }
   inline ERR addStream(FUNCTION Callback, FUNCTION OnStop, SFM SampleFormat, int SampleLength, int PlayOffset, struct AudioLoop * Loop, int LoopSize, int * Result) noexcept {
      struct snd::AddStream args = { Callback, OnStop, SampleFormat, SampleLength, PlayOffset, Loop, LoopSize, (int)0 };
      ERR error = Action(AC(-6), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }
   inline ERR beep(int Pitch, int Duration, int Volume) noexcept {
      struct snd::Beep args = { Pitch, Duration, Volume };
      return(Action(AC(-7), this, &args));
   }
   inline ERR setVolume(int Index, CSTRING Name, SVF Flags, int Channel, double Volume) noexcept {
      struct snd::SetVolume args = { Index, Name, Flags, Channel, Volume };
      return(Action(AC(-8), this, &args));
   }

   // Customised field setting

   inline ERR setOutputRate(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setInputRate(const int Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->InputRate = Value;
      return ERR::Okay;
   }

   inline ERR setQuality(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setFlags(const ADF Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setBitDepth(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setPeriods(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setPeriodSize(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   template <class T> inline ERR setDevice(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setMasterVolume(const double Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setMute(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setStereo(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

};

// Sound class definition

#define VER_SOUND (1.000000)

class objSound : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::SOUND;
   static constexpr CSTRING CLASS_NAME = "Sound";

   using create = pf::Create<objSound>;

   double   Volume;     // The volume to use when playing the sound sample.
   double   Pan;        // Determines the horizontal position of a sound when played through stereo speakers.
   int64_t  Position;   // The current playback position.
   int      Priority;   // The priority of a sound in relation to other sound samples being played.
   int      Length;     // Indicates the total byte-length of sample data.
   int      Octave;     // The octave to use for sample playback.
   SDF      Flags;      // Optional initialisation flags.
   int      Frequency;  // The frequency of a sampled sound is specified here.
   int      Playback;   // The playback frequency of the sound sample can be defined here.
   int      Compression; // Determines the amount of compression used when saving an audio sample.
   int      BytesPerSecond; // The flow of bytes-per-second when the sample is played at normal frequency.
   int      BitsPerSample; // Indicates the sample rate of the audio sample, typically 8 or 16 bit.
   OBJECTID AudioID;    // Refers to the audio object/device to use for playback.
   int      LoopStart;  // The byte position at which sample looping begins.
   int      LoopEnd;    // The byte position at which sample looping will end.
   STREAM   Stream;     // Defines the preferred streaming method for the sample.
   int      Handle;     // Audio handle acquired at the audio object [Private - Available to child classes]
   int      ChannelIndex; // Refers to the channel that the sound is playing through.

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR deactivate() noexcept { return Action(AC::Deactivate, this, nullptr); }
   inline ERR disable() noexcept { return Action(AC::Disable, this, nullptr); }
   inline ERR enable() noexcept { return Action(AC::Enable, this, nullptr); }
   inline ERR getKey(CSTRING Key, STRING Value, int Size) noexcept {
      struct acGetKey args = { Key, Value, Size };
      auto error = Action(AC::GetKey, this, &args);
      if ((error != ERR::Okay) and (Value)) Value[0] = 0;
      return error;
   }
   inline ERR init() noexcept { return InitObject(this); }
   template <class T, class U> ERR read(APTR Buffer, T Size, U *Result) noexcept {
      static_assert(std::is_integral<U>::value, "Result value must be an integer type");
      static_assert(std::is_integral<T>::value, "Size value must be an integer type");
      const int bytes = (Size > 0x7fffffff) ? 0x7fffffff : Size;
      struct acRead read = { (int8_t *)Buffer, bytes };
      if (auto error = Action(AC::Read, this, &read); error IS ERR::Okay) {
         *Result = static_cast<U>(read.Result);
         return ERR::Okay;
      }
      else { *Result = 0; return error; }
   }
   template <class T> ERR read(APTR Buffer, T Size) noexcept {
      static_assert(std::is_integral<T>::value, "Size value must be an integer type");
      const int bytes = (Size > 0x7fffffff) ? 0x7fffffff : Size;
      struct acRead read = { (int8_t *)Buffer, bytes };
      return Action(AC::Read, this, &read);
   }
   inline ERR saveToObject(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveToObject args = { Dest, { ClassID } };
      return Action(AC::SaveToObject, this, &args);
   }
   inline ERR seek(double Offset, SEEK Position = SEEK::CURRENT) noexcept {
      struct acSeek args = { Offset, Position };
      return Action(AC::Seek, this, &args);
   }
   inline ERR seekStart(double Offset) noexcept { return seek(Offset, SEEK::START); }
   inline ERR seekEnd(double Offset) noexcept { return seek(Offset, SEEK::END); }
   inline ERR seekCurrent(double Offset) noexcept { return seek(Offset, SEEK::CURRENT); }
   inline ERR acSetKey(CSTRING FieldName, CSTRING Value) noexcept {
      struct acSetKey args = { FieldName, Value };
      return Action(AC::SetKey, this, &args);
   }

   // Customised field setting

   inline ERR setVolume(const double Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setPan(const double Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setPosition(const int64_t Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(target, field, FD_INT64, &Value, 1);
   }

   inline ERR setPriority(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setLength(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setOctave(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setFlags(const SDF Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setFrequency(const int Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Frequency = Value;
      return ERR::Okay;
   }

   inline ERR setPlayback(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setCompression(const int Value) noexcept {
      this->Compression = Value;
      return ERR::Okay;
   }

   inline ERR setBytesPerSecond(const int Value) noexcept {
      this->BytesPerSecond = Value;
      return ERR::Okay;
   }

   inline ERR setBitsPerSample(const int Value) noexcept {
      this->BitsPerSample = Value;
      return ERR::Okay;
   }

   inline ERR setAudio(OBJECTID Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->AudioID = Value;
      return ERR::Okay;
   }

   inline ERR setLoopStart(const int Value) noexcept {
      this->LoopStart = Value;
      return ERR::Okay;
   }

   inline ERR setLoopEnd(const int Value) noexcept {
      this->LoopEnd = Value;
      return ERR::Okay;
   }

   inline ERR setStream(const STREAM Value) noexcept {
      this->Stream = Value;
      return ERR::Okay;
   }

   inline ERR setOnStop(FUNCTION Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   template <class T> inline ERR setPath(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[21];
      return field->WriteValue(target, field, 0x08800500, to_cstring(Value), 1);
   }

   template <class T> inline ERR setNote(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[20];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

};

#ifdef KOTUKU_STATIC
#define JUMPTABLE_AUDIO [[maybe_unused]] static struct AudioBase *AudioBase = nullptr;
#else
#define JUMPTABLE_AUDIO struct AudioBase *AudioBase = nullptr;
#endif

struct AudioBase {
#ifndef KOTUKU_STATIC
   ERR (*_MixContinue)(objAudio *Audio, int Handle);
   ERR (*_MixFrequency)(objAudio *Audio, int Handle, int Frequency);
   ERR (*_MixMute)(objAudio *Audio, int Handle, int Mute);
   ERR (*_MixPan)(objAudio *Audio, int Handle, double Pan);
   ERR (*_MixPlay)(objAudio *Audio, int Handle, int Position);
   ERR (*_MixRate)(objAudio *Audio, int Handle, int Rate);
   ERR (*_MixSample)(objAudio *Audio, int Handle, int Sample);
   ERR (*_MixStop)(objAudio *Audio, int Handle);
   ERR (*_MixStopLoop)(objAudio *Audio, int Handle);
   ERR (*_MixVolume)(objAudio *Audio, int Handle, double Volume);
   ERR (*_MixStartSequence)(objAudio *Audio, int Handle);
   ERR (*_MixEndSequence)(objAudio *Audio, int Handle);
#endif // KOTUKU_STATIC
};

#if !defined(KOTUKU_STATIC) and !defined(PRV_AUDIO_MODULE)
extern struct AudioBase *AudioBase;
namespace snd {
inline ERR MixContinue(objAudio *Audio, int Handle) { return AudioBase->_MixContinue(Audio,Handle); }
inline ERR MixFrequency(objAudio *Audio, int Handle, int Frequency) { return AudioBase->_MixFrequency(Audio,Handle,Frequency); }
inline ERR MixMute(objAudio *Audio, int Handle, int Mute) { return AudioBase->_MixMute(Audio,Handle,Mute); }
inline ERR MixPan(objAudio *Audio, int Handle, double Pan) { return AudioBase->_MixPan(Audio,Handle,Pan); }
inline ERR MixPlay(objAudio *Audio, int Handle, int Position) { return AudioBase->_MixPlay(Audio,Handle,Position); }
inline ERR MixRate(objAudio *Audio, int Handle, int Rate) { return AudioBase->_MixRate(Audio,Handle,Rate); }
inline ERR MixSample(objAudio *Audio, int Handle, int Sample) { return AudioBase->_MixSample(Audio,Handle,Sample); }
inline ERR MixStop(objAudio *Audio, int Handle) { return AudioBase->_MixStop(Audio,Handle); }
inline ERR MixStopLoop(objAudio *Audio, int Handle) { return AudioBase->_MixStopLoop(Audio,Handle); }
inline ERR MixVolume(objAudio *Audio, int Handle, double Volume) { return AudioBase->_MixVolume(Audio,Handle,Volume); }
inline ERR MixStartSequence(objAudio *Audio, int Handle) { return AudioBase->_MixStartSequence(Audio,Handle); }
inline ERR MixEndSequence(objAudio *Audio, int Handle) { return AudioBase->_MixEndSequence(Audio,Handle); }
} // namespace
#else
namespace snd {
extern ERR MixContinue(objAudio *Audio, int Handle);
extern ERR MixFrequency(objAudio *Audio, int Handle, int Frequency);
extern ERR MixMute(objAudio *Audio, int Handle, int Mute);
extern ERR MixPan(objAudio *Audio, int Handle, double Pan);
extern ERR MixPlay(objAudio *Audio, int Handle, int Position);
extern ERR MixRate(objAudio *Audio, int Handle, int Rate);
extern ERR MixSample(objAudio *Audio, int Handle, int Sample);
extern ERR MixStop(objAudio *Audio, int Handle);
extern ERR MixStopLoop(objAudio *Audio, int Handle);
extern ERR MixVolume(objAudio *Audio, int Handle, double Volume);
extern ERR MixStartSequence(objAudio *Audio, int Handle);
extern ERR MixEndSequence(objAudio *Audio, int Handle);
} // namespace
#endif // KOTUKU_STATIC

