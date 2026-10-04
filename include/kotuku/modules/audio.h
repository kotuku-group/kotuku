#pragma once

// Name:      audio.h
// Copyright: Paul Manias © 2002-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_AUDIO (1)

class objAudio;
class objAudioEffect;
class objAudioEqualiser;
class objAudioReverb;
class objAudioCompressor;
class objAudioLimiter;
class objAudioGate;
class objAudioDelay;
class objAudioChorus;
class objAudioFlanger;
class objAudioSaturator;
class objAudioSplitter;
class objAudioAnalyser;
class objSound;

// Optional flags for the Audio object.

enum class ADF : uint32_t {
   NIL = 0,
   OVER_SAMPLING = 0x00000001,
   FILTER_LOW = 0x00000002,
   FILTER_HIGH = 0x00000004,
   VOL_RAMPING = 0x00000008,
   AUTO_SAVE = 0x00000010,
   SYSTEM_WIDE = 0x00000020,
};

DEFINE_ENUM_FLAG_OPERATORS(ADF)

// Audio effect flags.

enum class AEF : uint32_t {
   NIL = 0,
   BYPASS = 0x00000001,
};

DEFINE_ENUM_FLAG_OPERATORS(AEF)

// Audio meter snapshot flags.

enum class AMF : uint32_t {
   NIL = 0,
   VALID = 0x00000001,
   IDLE = 0x00000002,
   BYPASSED = 0x00000004,
   DISCONNECTED = 0x00000008,
   NO_SAMPLES = 0x00000010,
};

DEFINE_ENUM_FLAG_OPERATORS(AMF)

// Per-value flags returned by AudioEffect.ReadMeters().

enum class AMV : uint32_t {
   NIL = 0,
   VALID = 0x00000001,
   FLOOR = 0x00000002,
};

DEFINE_ENUM_FLAG_OPERATORS(AMV)

// Sample representations, independent of the channel count.

enum class ASF : int {
   NIL = 0,
   U8 = 1,
   S16 = 2,
   F32 = 3,
};

// Optional AudioFormat flags.

enum class AFF : uint32_t {
   NIL = 0,
   BIG_ENDIAN_ORDER = 0x00000001,
};

DEFINE_ENUM_FLAG_OPERATORS(AFF)

// Channel identities for the Layout of an AudioFormat.

enum class SPK : int {
   NIL = 0,
   DISCRETE = 0x10000,
   CENTRE = 1,
   FRONT_LEFT = 2,
   FRONT_RIGHT = 3,
   LFE = 4,
   SIDE_LEFT = 5,
   SIDE_RIGHT = 6,
   REAR_LEFT = 7,
   REAR_RIGHT = 8,
};

// Named channel layout presets for ExpandLayout().

enum class ACL : int {
   NIL = 0,
   MONO = 1,
   STEREO = 2,
   SURROUND_5_1_SIDE = 3,
   SURROUND_5_1_REAR = 4,
};

// Availability of a reported processing format.

enum class AFS : int {
   NIL = 0,
   UNAVAILABLE = 0,
   ACTIVE = 1,
   INACTIVE = 2,
};

// Effect drain states reported by GetEffectStatus().

enum class ADS : int {
   NIL = 0,
   IDLE = 0,
   ACTIVE = 1,
   DRAINING = 2,
};

// Parametric equaliser band types.

enum class EQB : int {
   NIL = 0,
   PEAK = 0,
   LOW_SHELF = 1,
   HIGH_SHELF = 2,
   LOW_PASS = 3,
   HIGH_PASS = 4,
};

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
   RESTRICT_PLAY = 0x00000004,
   STREAM = 0x40000000,
   NOTE = 0x80000000,
};

DEFINE_ENUM_FLAG_OPERATORS(SDF)

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

// Mixer batch operations.

enum class MIX : int {
   NIL = 0,
   CONTINUE = 1,
   FREQUENCY = 2,
   MUTE = 3,
   PAN = 4,
   PAUSE = 5,
   PLAY = 6,
   RELEASE = 7,
   TEMPO = 8,
   SAMPLE = 9,
   STOP = 10,
   VOLUME = 11,
};

struct AudioMixCommand {
   MIX     Operation; // Mixer operation
   int     Handle;   // Target channel; all commands in a batch must use the same channel set
   int64_t Integer;  // Integer argument for the selected operation; otherwise ignored
   double  Value;    // Finite floating-point argument for PAN and VOLUME; otherwise ignored
};

struct AudioLoop {
   LOOP    LoopMode;      // Loop mode (single, double)
   LTYPE   Loop1Type;     // First loop type (unidirectional, bidirectional)
   LTYPE   Loop2Type;     // Second loop type (unidirectional, bidirectional)
   int64_t Loop1Start;    // Byte position at the start of the first loop
   int64_t Loop1End;      // Byte position at the end of the first loop
   int64_t Loop2Start;    // Byte position at the start of the second loop
   int64_t Loop2End;      // Byte position at the end of the second loop
};

struct AudioFormat {
   int SampleRate;            // Nominal sample rate in frames per second
   ASF SampleFormat;          // Representation of each sample
   AFF Flags;                 // Optional format flags
   kt::vector<int> Layout;    // Ordered channel identities from SPK; the length is the channel count
};

struct MeterInfo {
   std::string Key;          // Stable key, e.g. input_peak_left
   std::string Label;        // Human-readable label
   std::string Unit;         // Unit of measurement, e.g. dBFS, or empty
   std::string Scope;        // Either channel or global
   std::string Semantics;    // Kind of measurement, e.g. sample-peak, or empty
   int Slot;                 // Index of the value in the Values of a MeterReading
   int Channel;              // Channel identity from SPK, or zero for a global value
};

struct MeterLayout {
   int64_t ID;                      // Configuration identifier shared with the matching MeterReading
   kt::vector<MeterInfo> Meters;    // Meter descriptors in slot order
};

struct MeterValue {
   double Value;    // Measured value in the units of its MeterInfo descriptor
   AMV    Flags;    // Validity of the value
};

struct MeterReading {
   int64_t Sequence;                 // Publication sequence number; increases with each published interval
   int64_t ID;                       // Configuration identifier; compare with the ID from GetMeterLayout()
   int64_t Position;                 // Processed-frame position at the end of the interval
   AMF     Flags;                    // Validity and lifecycle state of the reading
   int     Interval;                 // Length of the measurement interval in frames
   kt::vector<MeterValue> Values;    // One value per meter descriptor, indexed by slot
};

struct AudioEQBand {
   EQB    Type;         // Filter type
   double Frequency;    // Centre or corner frequency in hertz
   double Gain;         // Band gain in decibels
   double Q;            // Quality factor or shelf slope
};

// Audio class definition

#define VER_AUDIO (1.000000)

// Audio methods

namespace snd {
struct OpenChannels { int Total; int Result; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CloseChannels { int Handle; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct AddSample { FUNCTION OnStop; struct AudioFormat *Format; std::span<const int8_t> Data; struct AudioLoop *Loop; int Result; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RemoveSample { int Handle; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetSampleLength { int Sample; int64_t Length; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct AddStream { FUNCTION Callback; FUNCTION OnStop; struct AudioFormat *Format; int64_t SampleLength; int64_t PlayOffset; struct AudioLoop *Loop; int Result; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Beep { int Pitch; int Duration; int Volume; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetVolume { int Index; std::string_view Name; SVF Flags; int Channel; double Volume; static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetEffectStatus { int Channel; int64_t Application; int64_t Global; int64_t Total; int Rate; int64_t Generation; ADS State; int Truncated; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ResetEffects { int Channel; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetOutputFormat { int SampleRate; ASF SampleFormat; kt::vector<int> *Layout; int64_t Generation; AFS State; static const AC id = AC(-11); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetVolumeChannels { int Index; std::string_view Name; kt::vector<int> *Channels; kt::vector<int> *Speakers; VCF Flags; static const AC id = AC(-12); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objAudio : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::AUDIO;
   static constexpr CSTRING CLASS_NAME = "Audio";

   using create = kt::Create<objAudio>;
   objAudio(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   int OutputRate;    // Determines the frequency to use for the output of audio data.
   int InputRate;     // Determines the frequency to use when recording audio data.
   int Quality;       // Determines the quality of the audio mixing.
   ADF Flags;         // Special audio flags can be set here.
   int BitDepth;      // The bit depth affects the overall quality of audio input and output.
   int Periods;       // Defines the number of periods that make up the internal audio buffer.
   int PeriodSize;    // Defines the number of frames in each ALSA period.

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
      return error;
   }
   inline ERR closeChannels(int Handle) noexcept {
      struct snd::CloseChannels args = { Handle };
      return Action(AC(-2), this, &args);
   }
   inline ERR addSample(FUNCTION OnStop, struct AudioFormat * Format, std::span<const int8_t> Data, struct AudioLoop * Loop, int * Result) noexcept {
      struct snd::AddSample args = { OnStop, Format, Data, Loop, (int)0 };
      ERR error = Action(AC(-3), this, &args);
      if (Result) *Result = args.Result;
      return error;
   }
   inline ERR removeSample(int Handle) noexcept {
      struct snd::RemoveSample args = { Handle };
      return Action(AC(-4), this, &args);
   }
   inline ERR setSampleLength(int Sample, int64_t Length) noexcept {
      struct snd::SetSampleLength args = { Sample, Length };
      return Action(AC(-5), this, &args);
   }
   inline ERR addStream(FUNCTION Callback, FUNCTION OnStop, struct AudioFormat * Format, int64_t SampleLength, int64_t PlayOffset, struct AudioLoop * Loop, int * Result) noexcept {
      struct snd::AddStream args = { Callback, OnStop, Format, SampleLength, PlayOffset, Loop, (int)0 };
      ERR error = Action(AC(-6), this, &args);
      if (Result) *Result = args.Result;
      return error;
   }
   inline ERR beep(int Pitch, int Duration, int Volume) noexcept {
      struct snd::Beep args = { Pitch, Duration, Volume };
      return Action(AC(-7), this, &args);
   }
   inline ERR setVolume(int Index, const std::string_view &Name, SVF Flags, int Channel, double Volume) noexcept {
      struct snd::SetVolume args = { Index, Name, Flags, Channel, Volume };
      return Action(AC(-8), this, &args);
   }
   inline ERR getEffectStatus(int Channel, int64_t * Application, int64_t * Global, int64_t * Total, int * Rate, int64_t * Generation, ADS * State, int * Truncated) noexcept {
      struct snd::GetEffectStatus args = { Channel, (int64_t)0, (int64_t)0, (int64_t)0, (int)0, (int64_t)0, (ADS)0, (int)0 };
      ERR error = Action(AC(-9), this, &args);
      if (Application) *Application = args.Application;
      if (Global) *Global = args.Global;
      if (Total) *Total = args.Total;
      if (Rate) *Rate = args.Rate;
      if (Generation) *Generation = args.Generation;
      if (State) *State = args.State;
      if (Truncated) *Truncated = args.Truncated;
      return error;
   }
   inline ERR resetEffects(int Channel) noexcept {
      struct snd::ResetEffects args = { Channel };
      return Action(AC(-10), this, &args);
   }
   inline ERR getOutputFormat(int * SampleRate, ASF * SampleFormat, kt::vector<int> &Layout, int64_t * Generation, AFS * State) noexcept {
      struct snd::GetOutputFormat args = { (int)0, (ASF)0, &Layout, (int64_t)0, (AFS)0 };
      ERR error = Action(AC(-11), this, &args);
      if (SampleRate) *SampleRate = args.SampleRate;
      if (SampleFormat) *SampleFormat = args.SampleFormat;
      if (Generation) *Generation = args.Generation;
      if (State) *State = args.State;
      return error;
   }
   inline ERR getVolumeChannels(int Index, const std::string_view &Name, kt::vector<int> &Channels, kt::vector<int> &Speakers, VCF * Flags) noexcept {
      struct snd::GetVolumeChannels args = { Index, Name, &Channels, &Speakers, (VCF)0 };
      ERR error = Action(AC(-12), this, &args);
      if (Flags) *Flags = args.Flags;
      return error;
   }

   // Customised field getting

   inline ERR getMaxDrain(double &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getOutputLayout(std::span<int> &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      auto get_field = (ERR (*)(APTR, std::span<int> &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getOutputRate(int &Value) noexcept {
      Value = this->OutputRate;
      return ERR::Okay;
   }

   inline ERR getInputRate(int &Value) noexcept {
      Value = this->InputRate;
      return ERR::Okay;
   }

   inline ERR getQuality(int &Value) noexcept {
      Value = this->Quality;
      return ERR::Okay;
   }

   inline ERR getFlags(ADF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getBitDepth(int &Value) noexcept {
      Value = this->BitDepth;
      return ERR::Okay;
   }

   inline ERR getPeriods(int &Value) noexcept {
      Value = this->Periods;
      return ERR::Okay;
   }

   inline ERR getPeriodSize(int &Value) noexcept {
      Value = this->PeriodSize;
      return ERR::Okay;
   }

   inline ERR getDevice(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getMixerLag(double &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getMasterVolume(double &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->GetValue(this, &Value);
   }

   inline ERR getMute(int &Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setMaxDrain(const double Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_DOUBLE, &Value);
   }

   inline ERR setOutputLayout(std::span<const int> Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, 0x40101308, &Value);
   }

   inline ERR setOutputRate(const int Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setInputRate(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->InputRate = Value;
      return ERR::Okay;
   }

   inline ERR setQuality(const int Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setFlags(const ADF Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setBitDepth(const int Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setPeriods(const int Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setPeriodSize(const int Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setDevice(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, 0x00904300, &Value);
   }

   inline ERR setMasterVolume(const double Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_DOUBLE, &Value);
   }

   inline ERR setMute(const int Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

};

// AudioEffect class definition

#define VER_AUDIOEFFECT (1.000000)

// AudioEffect methods

namespace fx {
struct InsertEntry { std::string_view Group; int Index; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RemoveEntry { std::string_view Group; int Index; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetResponse { std::span<const double> Frequencies; std::span<double> Magnitudes; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetGroupCount { std::string_view Group; int Count; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetMeterLayout { struct MeterLayout *Layout; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetOutput { std::string_view Key; double Value; static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ReadMeters { struct MeterReading *Reading; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetProcessingFormat { int SampleRate; ASF SampleFormat; kt::vector<int> *Layout; int64_t Generation; AFS State; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objAudioEffect : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::AUDIOEFFECT;
   static constexpr CSTRING CLASS_NAME = "AudioEffect";

   using create = kt::Create<objAudioEffect>;
   objAudioEffect(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   OBJECTID AudioID;  // Target Audio object, inherited from an Audio owner if omitted.
   int      Channel;  // Channel-set handle, or zero for the global chain.
   int      Order;    // Processing position within the chain.
   AEF      Flags;    // Optional processing flags.
   int      OutputRate; // Output sample rate of the attached Audio object.
   OBJECTID ParentID; // The container effect that hosts this effect, or zero for an effect in a top-level chain.
   int      Branch;   // Zero-based index of the container branch that hosts the effect.

   // Action stubs

   inline ERR flush() noexcept { return Action(AC::Flush, this, nullptr); }
   inline ERR getKey(std::string_view Key, std::string &Value) noexcept {
      struct acGetKey args = { Key, &Value };
      auto error = Action(AC::GetKey, this, &args);
      if (error != ERR::Okay) Value.clear();
      return error;
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR acSetKey(std::string_view FieldName, std::string_view Value) noexcept {
      struct acSetKey args = { FieldName, Value };
      return Action(AC::SetKey, this, &args);
   }
   inline ERR insertEntry(const std::string_view &Group, int Index) noexcept {
      struct fx::InsertEntry args = { Group, Index };
      return Action(AC(-3), this, &args);
   }
   inline ERR removeEntry(const std::string_view &Group, int Index) noexcept {
      struct fx::RemoveEntry args = { Group, Index };
      return Action(AC(-4), this, &args);
   }
   inline ERR getResponse(std::span<const double> Frequencies, std::span<double> Magnitudes) noexcept {
      struct fx::GetResponse args = { Frequencies, Magnitudes };
      return Action(AC(-5), this, &args);
   }
   inline ERR getGroupCount(const std::string_view &Group, int * Count) noexcept {
      struct fx::GetGroupCount args = { Group, (int)0 };
      ERR error = Action(AC(-6), this, &args);
      if (Count) *Count = args.Count;
      return error;
   }
   inline ERR getMeterLayout(struct MeterLayout ** Layout) noexcept {
      struct fx::GetMeterLayout args = { (struct MeterLayout *)0 };
      ERR error = Action(AC(-7), this, &args);
      if (Layout) *Layout = args.Layout;
      return error;
   }
   inline ERR getOutput(const std::string_view &Key, double * Value) noexcept {
      struct fx::GetOutput args = { Key, (double)0 };
      ERR error = Action(AC(-8), this, &args);
      if (Value) *Value = args.Value;
      return error;
   }
   inline ERR readMeters(struct MeterReading ** Reading) noexcept {
      struct fx::ReadMeters args = { (struct MeterReading *)0 };
      ERR error = Action(AC(-9), this, &args);
      if (Reading) *Reading = args.Reading;
      return error;
   }
   inline ERR getProcessingFormat(int * SampleRate, ASF * SampleFormat, kt::vector<int> &Layout, int64_t * Generation, AFS * State) noexcept {
      struct fx::GetProcessingFormat args = { (int)0, (ASF)0, &Layout, (int64_t)0, (AFS)0 };
      ERR error = Action(AC(-10), this, &args);
      if (SampleRate) *SampleRate = args.SampleRate;
      if (SampleFormat) *SampleFormat = args.SampleFormat;
      if (Generation) *Generation = args.Generation;
      if (State) *State = args.State;
      return error;
   }

   // Customised field getting

   inline ERR getAudio(OBJECTID &Value) noexcept {
      Value = this->AudioID;
      return ERR::Okay;
   }

   inline ERR getChannel(int &Value) noexcept {
      Value = this->Channel;
      return ERR::Okay;
   }

   inline ERR getOrder(int &Value) noexcept {
      Value = this->Order;
      return ERR::Okay;
   }

   inline ERR getFlags(AEF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getOutputRate(int &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getParent(OBJECTID &Value) noexcept {
      Value = this->ParentID;
      return ERR::Okay;
   }

   inline ERR getBranch(int &Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getLatency(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getMutable(int &Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getSchema(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setAudio(OBJECTID Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->AudioID = Value;
      return ERR::Okay;
   }

   inline ERR setChannel(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Channel = Value;
      return ERR::Okay;
   }

   inline ERR setOrder(const int Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setFlags(const AEF Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setParent(OBJECTID Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->ParentID = Value;
      return ERR::Okay;
   }

   inline ERR setBranch(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Branch = Value;
      return ERR::Okay;
   }

};

// AudioEqualiser class definition

#define VER_AUDIOEQUALISER (1.000000)

class objAudioEqualiser : public objAudioEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::AUDIOEQUALISER;
   static constexpr CSTRING CLASS_NAME = "AudioEqualiser";

   using create = kt::Create<objAudioEqualiser>;
   objAudioEqualiser(objMetaClass *pClass, OBJECTID pUID) noexcept : objAudioEffect(pClass, pUID) {}

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getBands(std::span<struct AudioEQBand> &Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      auto get_field = (ERR (*)(APTR, std::span<struct AudioEQBand> &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getGain(double &Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->GetValue(this, &Value);
   }


   // Customised field setting

   inline ERR setBands(std::span<const struct AudioEQBand> Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(this, field, 0x00101318, &Value);
   }

   inline ERR setGain(const double Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(this, field, FD_DOUBLE, &Value);
   }

};

// AudioReverb class definition

#define VER_AUDIOREVERB (1.000000)

class objAudioReverb : public objAudioEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::AUDIOREVERB;
   static constexpr CSTRING CLASS_NAME = "AudioReverb";

   using create = kt::Create<objAudioReverb>;
   objAudioReverb(objMetaClass *pClass, OBJECTID pUID) noexcept : objAudioEffect(pClass, pUID) {}

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting


   // Customised field setting

};

// AudioCompressor class definition

#define VER_AUDIOCOMPRESSOR (1.000000)

class objAudioCompressor : public objAudioEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::AUDIOCOMPRESSOR;
   static constexpr CSTRING CLASS_NAME = "AudioCompressor";

   using create = kt::Create<objAudioCompressor>;
   objAudioCompressor(objMetaClass *pClass, OBJECTID pUID) noexcept : objAudioEffect(pClass, pUID) {}

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting


   // Customised field setting

};

// AudioLimiter class definition

#define VER_AUDIOLIMITER (1.000000)

class objAudioLimiter : public objAudioEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::AUDIOLIMITER;
   static constexpr CSTRING CLASS_NAME = "AudioLimiter";

   using create = kt::Create<objAudioLimiter>;
   objAudioLimiter(objMetaClass *pClass, OBJECTID pUID) noexcept : objAudioEffect(pClass, pUID) {}

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting


   // Customised field setting

};

// AudioGate class definition

#define VER_AUDIOGATE (1.000000)

class objAudioGate : public objAudioEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::AUDIOGATE;
   static constexpr CSTRING CLASS_NAME = "AudioGate";

   using create = kt::Create<objAudioGate>;
   objAudioGate(objMetaClass *pClass, OBJECTID pUID) noexcept : objAudioEffect(pClass, pUID) {}

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting


   // Customised field setting

};

// AudioDelay class definition

#define VER_AUDIODELAY (1.000000)

class objAudioDelay : public objAudioEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::AUDIODELAY;
   static constexpr CSTRING CLASS_NAME = "AudioDelay";

   using create = kt::Create<objAudioDelay>;
   objAudioDelay(objMetaClass *pClass, OBJECTID pUID) noexcept : objAudioEffect(pClass, pUID) {}

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting


   // Customised field setting

};

// AudioChorus class definition

#define VER_AUDIOCHORUS (1.000000)

class objAudioChorus : public objAudioEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::AUDIOCHORUS;
   static constexpr CSTRING CLASS_NAME = "AudioChorus";

   using create = kt::Create<objAudioChorus>;
   objAudioChorus(objMetaClass *pClass, OBJECTID pUID) noexcept : objAudioEffect(pClass, pUID) {}

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting


   // Customised field setting

};

// AudioFlanger class definition

#define VER_AUDIOFLANGER (1.000000)

class objAudioFlanger : public objAudioEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::AUDIOFLANGER;
   static constexpr CSTRING CLASS_NAME = "AudioFlanger";

   using create = kt::Create<objAudioFlanger>;
   objAudioFlanger(objMetaClass *pClass, OBJECTID pUID) noexcept : objAudioEffect(pClass, pUID) {}

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting


   // Customised field setting

};

// AudioSaturator class definition

#define VER_AUDIOSATURATOR (1.000000)

class objAudioSaturator : public objAudioEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::AUDIOSATURATOR;
   static constexpr CSTRING CLASS_NAME = "AudioSaturator";

   using create = kt::Create<objAudioSaturator>;
   objAudioSaturator(objMetaClass *pClass, OBJECTID pUID) noexcept : objAudioEffect(pClass, pUID) {}

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting


   // Customised field setting

};

// AudioSplitter class definition

#define VER_AUDIOSPLITTER (1.000000)

class objAudioSplitter : public objAudioEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::AUDIOSPLITTER;
   static constexpr CSTRING CLASS_NAME = "AudioSplitter";

   using create = kt::Create<objAudioSplitter>;
   objAudioSplitter(objMetaClass *pClass, OBJECTID pUID) noexcept : objAudioEffect(pClass, pUID) {}

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getLatencyBudget(double &Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setLatencyBudget(const double Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(this, field, FD_DOUBLE, &Value);
   }

};

// AudioAnalyser class definition

#define VER_AUDIOANALYSER (1.000000)

// AudioAnalyser methods

namespace ana {
struct GetSpectrum { std::span<const double> Frequencies; std::span<double> Levels; static const AC id = AC(-30); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetWaveform { std::span<float> Samples; static const AC id = AC(-31); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objAudioAnalyser : public objAudioEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::AUDIOANALYSER;
   static constexpr CSTRING CLASS_NAME = "AudioAnalyser";

   using create = kt::Create<objAudioAnalyser>;
   objAudioAnalyser(objMetaClass *pClass, OBJECTID pUID) noexcept : objAudioEffect(pClass, pUID) {}

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
   inline ERR getSpectrum(std::span<const double> Frequencies, std::span<double> Levels) noexcept {
      struct ana::GetSpectrum args = { Frequencies, Levels };
      return Action(AC(-30), this, &args);
   }
   inline ERR getWaveform(std::span<float> Samples) noexcept {
      struct ana::GetWaveform args = { Samples };
      return Action(AC(-31), this, &args);
   }

   // Customised field getting


   // Customised field setting

};

// Sound class definition

#define VER_SOUND (1.000000)

class objSound : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::SOUND;
   static constexpr CSTRING CLASS_NAME = "Sound";

   using create = kt::Create<objSound>;
   objSound(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   std::string Path;    // Location of the audio sample data.
   double   Volume;     // The volume to use when playing the sound sample.
   double   Pan;        // Determines the horizontal position of a sound when played through stereo speakers.
   int64_t  Position;   // The current playback position.
   int64_t  Length;     // Indicates the total byte-length of sample data.
   int64_t  LoopStart;  // The byte position at which sample looping begins.
   int64_t  LoopEnd;    // The byte position at which sample looping will end.
   int      Priority;   // The priority of a sound in relation to other sound samples being played.
   int      Octave;     // The octave to use for sample playback.
   SDF      Flags;      // Optional initialisation flags.
   int      Playback;   // The playback frequency of the sound sample can be defined here.
   int      Compression; // Determines the amount of compression used when saving an audio sample.
   int      BytesPerSecond; // The flow of bytes-per-second when the sample is played at normal frequency.
   OBJECTID AudioID;    // Refers to the audio object/device to use for playback.
   STREAM   Stream;     // Defines the preferred streaming method for the sample.
   int      Handle;     // Audio handle acquired at the audio object [Private - Available to child classes]
   int      ChannelIndex; // Refers to the channel that the sound is playing through.

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR deactivate() noexcept { return Action(AC::Deactivate, this, nullptr); }
   inline ERR disable() noexcept { return Action(AC::Disable, this, nullptr); }
   inline ERR enable() noexcept { return Action(AC::Enable, this, nullptr); }
   inline ERR getKey(std::string_view Key, std::string &Value) noexcept {
      struct acGetKey args = { Key, &Value };
      auto error = Action(AC::GetKey, this, &args);
      if (error != ERR::Okay) Value.clear();
      return error;
   }
   inline ERR init() noexcept { return InitObject(this); }
   template <class T> ERR read(std::span<int8_t> Buffer, T *Result) noexcept {
      static_assert(std::is_integral<T>::value, "Result value must be an integer type");
      struct acRead read = { Buffer };
      if (auto error = Action(AC::Read, this, &read); error IS ERR::Okay) {
         *Result = T(read.Result);
         return ERR::Okay;
      }
      else { *Result = 0; return error; }
   }
   inline ERR read(std::span<int8_t> Buffer) noexcept {
      struct acRead read = { Buffer };
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
   inline ERR acSetKey(std::string_view FieldName, std::string_view Value) noexcept {
      struct acSetKey args = { FieldName, Value };
      return Action(AC::SetKey, this, &args);
   }

   // Customised field getting

   inline ERR getSourceFormat(struct AudioFormat * &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->GetValue(this, &Value);
   }

   inline ERR getChannels(int &Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      return field->GetValue(this, &Value);
   }

   inline ERR getFrameBytes(int &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->GetValue(this, &Value);
   }

   inline ERR getSampleRate(int &Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->GetValue(this, &Value);
   }

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getVolume(double &Value) noexcept {
      Value = this->Volume;
      return ERR::Okay;
   }

   inline ERR getPan(double &Value) noexcept {
      Value = this->Pan;
      return ERR::Okay;
   }

   inline ERR getPosition(int64_t &Value) noexcept {
      Value = this->Position;
      return ERR::Okay;
   }

   inline ERR getLength(int64_t &Value) noexcept {
      Value = this->Length;
      return ERR::Okay;
   }

   inline ERR getLoopStart(int64_t &Value) noexcept {
      Value = this->LoopStart;
      return ERR::Okay;
   }

   inline ERR getLoopEnd(int64_t &Value) noexcept {
      Value = this->LoopEnd;
      return ERR::Okay;
   }

   inline ERR getPriority(int &Value) noexcept {
      Value = this->Priority;
      return ERR::Okay;
   }

   inline ERR getOctave(int &Value) noexcept {
      Value = this->Octave;
      return ERR::Okay;
   }

   inline ERR getFlags(SDF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getPlayback(int &Value) noexcept {
      Value = this->Playback;
      return ERR::Okay;
   }

   inline ERR getCompression(int &Value) noexcept {
      Value = this->Compression;
      return ERR::Okay;
   }

   inline ERR getBytesPerSecond(int &Value) noexcept {
      Value = this->BytesPerSecond;
      return ERR::Okay;
   }

   inline ERR getAudio(OBJECTID &Value) noexcept {
      Value = this->AudioID;
      return ERR::Okay;
   }

   inline ERR getStream(STREAM &Value) noexcept {
      Value = this->Stream;
      return ERR::Okay;
   }

   inline ERR getChannelIndex(int &Value) noexcept {
      Value = this->ChannelIndex;
      return ERR::Okay;
   }

   inline ERR getActive(int &Value) noexcept {
      auto field = &this->Class->Dictionary[23];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getDuration(double &Value) noexcept {
      auto field = &this->Class->Dictionary[33];
      return field->GetValue(this, &Value);
   }

   inline ERR getElapsed(double &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getHeader(std::span<int8_t> &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto get_field = (ERR (*)(APTR, std::span<int8_t> &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getOnStop(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[31];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getPlayPosition(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getProgress(double &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getRemaining(double &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getNote(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[32];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setSourceFormat(struct AudioFormat * Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, 0x08100518, Value);
   }

   inline ERR setPath(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Path = Value;
      return ERR::Okay;
   }

   inline ERR setVolume(const double Value) noexcept {
      auto field = &this->Class->Dictionary[30];
      return field->WriteValue(this, field, FD_DOUBLE, &Value);
   }

   inline ERR setPan(const double Value) noexcept {
      auto field = &this->Class->Dictionary[20];
      return field->WriteValue(this, field, FD_DOUBLE, &Value);
   }

   inline ERR setPosition(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[18];
      return field->WriteValue(this, field, FD_INT64, &Value);
   }

   inline ERR setLength(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[35];
      return field->WriteValue(this, field, FD_INT64, &Value);
   }

   inline ERR setLoopStart(const int64_t Value) noexcept {
      this->LoopStart = Value;
      return ERR::Okay;
   }

   inline ERR setLoopEnd(const int64_t Value) noexcept {
      this->LoopEnd = Value;
      return ERR::Okay;
   }

   inline ERR setPriority(const int Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setOctave(const int Value) noexcept {
      auto field = &this->Class->Dictionary[27];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setFlags(const SDF Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setPlayback(const int Value) noexcept {
      auto field = &this->Class->Dictionary[24];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setCompression(const int Value) noexcept {
      this->Compression = Value;
      return ERR::Okay;
   }

   inline ERR setAudio(OBJECTID Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->AudioID = Value;
      return ERR::Okay;
   }

   inline ERR setStream(const STREAM Value) noexcept {
      this->Stream = Value;
      return ERR::Okay;
   }

   inline ERR setOnStop(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[31];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setNote(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[32];
      return field->WriteValue(this, field, 0x00804308, &Value);
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
   ERR (*_MixPause)(objAudio *Audio, int Handle);
   ERR (*_MixPlay)(objAudio *Audio, int Handle, int64_t Position);
   ERR (*_MixRelease)(objAudio *Audio, int Handle);
   ERR (*_MixTempo)(objAudio *Audio, int Handle, int Tempo);
   ERR (*_MixSample)(objAudio *Audio, int Handle, int Sample);
   ERR (*_MixStop)(objAudio *Audio, int Handle);
   ERR (*_MixVolume)(objAudio *Audio, int Handle, double Volume);
   ERR (*_MixSubmitBatch)(objAudio *Audio, const std::span<const struct AudioMixCommand> &Commands, FUNCTION *OnComplete);
   ERR (*_ExpandLayout)(ACL Layout, kt::vector<int> *Channels);
   ERR (*_GetFrameBytes)(struct AudioFormat *Format, int *Bytes);
#endif // KOTUKU_STATIC
};

#if !defined(KOTUKU_STATIC) and !defined(PRV_AUDIO_MODULE)
extern struct AudioBase *AudioBase;
namespace snd {
inline ERR MixContinue(objAudio *Audio, int Handle) { return AudioBase->_MixContinue(Audio,Handle); }
inline ERR MixFrequency(objAudio *Audio, int Handle, int Frequency) { return AudioBase->_MixFrequency(Audio,Handle,Frequency); }
inline ERR MixMute(objAudio *Audio, int Handle, int Mute) { return AudioBase->_MixMute(Audio,Handle,Mute); }
inline ERR MixPan(objAudio *Audio, int Handle, double Pan) { return AudioBase->_MixPan(Audio,Handle,Pan); }
inline ERR MixPause(objAudio *Audio, int Handle) { return AudioBase->_MixPause(Audio,Handle); }
inline ERR MixPlay(objAudio *Audio, int Handle, int64_t Position) { return AudioBase->_MixPlay(Audio,Handle,Position); }
inline ERR MixRelease(objAudio *Audio, int Handle) { return AudioBase->_MixRelease(Audio,Handle); }
inline ERR MixTempo(objAudio *Audio, int Handle, int Tempo) { return AudioBase->_MixTempo(Audio,Handle,Tempo); }
inline ERR MixSample(objAudio *Audio, int Handle, int Sample) { return AudioBase->_MixSample(Audio,Handle,Sample); }
inline ERR MixStop(objAudio *Audio, int Handle) { return AudioBase->_MixStop(Audio,Handle); }
inline ERR MixVolume(objAudio *Audio, int Handle, double Volume) { return AudioBase->_MixVolume(Audio,Handle,Volume); }
inline ERR MixSubmitBatch(objAudio *Audio, const std::span<const struct AudioMixCommand> &Commands, FUNCTION *OnComplete) { return AudioBase->_MixSubmitBatch(Audio,Commands,OnComplete); }
inline ERR ExpandLayout(ACL Layout, kt::vector<int> *Channels) { return AudioBase->_ExpandLayout(Layout,Channels); }
inline ERR GetFrameBytes(struct AudioFormat *Format, int *Bytes) { return AudioBase->_GetFrameBytes(Format,Bytes); }
} // namespace
#else
namespace snd {
extern ERR MixContinue(objAudio *Audio, int Handle);
extern ERR MixFrequency(objAudio *Audio, int Handle, int Frequency);
extern ERR MixMute(objAudio *Audio, int Handle, int Mute);
extern ERR MixPan(objAudio *Audio, int Handle, double Pan);
extern ERR MixPause(objAudio *Audio, int Handle);
extern ERR MixPlay(objAudio *Audio, int Handle, int64_t Position);
extern ERR MixRelease(objAudio *Audio, int Handle);
extern ERR MixTempo(objAudio *Audio, int Handle, int Tempo);
extern ERR MixSample(objAudio *Audio, int Handle, int Sample);
extern ERR MixStop(objAudio *Audio, int Handle);
extern ERR MixVolume(objAudio *Audio, int Handle, double Volume);
extern ERR MixSubmitBatch(objAudio *Audio, const std::span<const struct AudioMixCommand> &Commands, FUNCTION *OnComplete);
extern ERR ExpandLayout(ACL Layout, kt::vector<int> *Channels);
extern ERR GetFrameBytes(struct AudioFormat *Format, int *Bytes);
} // namespace
#endif // KOTUKU_STATIC