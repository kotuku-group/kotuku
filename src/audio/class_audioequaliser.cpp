/*********************************************************************************************************************

-CLASS-
AudioEqualiser: A parametric equaliser for an Audio mixing chain.

Create as a child of an Audio object or set the inherited Audio field.  Use Channel to process one channel set, or
leave it at zero to process the global mix.  Bands run in list order.

The #Bands and #Gain form one configuration that is applied to every channel of the processing layout, with
independent filter history for each channel.  The equaliser does not perform bass management and is not an automatic
crossover for an LFE channel.  Application equalisers accept live changes;
global equalisers become immutable after initialisation.

The top-level parameters are available as the fields #Gain, #Loudness and #ListeningLevel.  Each band is an
!AudioEQBand with a type, frequency, gain and Q, and up to 64 bands are permitted.  Band gain is ignored by pass
filters, and Q is limited to one for shelf filters, where it sets the shelf slope.  Use #InsertBand(), #SetBand() and
#RemoveBand() to edit individual bands, #GetBand() to read one, and #Bands to read or replace the whole list.

Field writes and band methods before initialisation are range-checked and stored, and `Init()` validates the complete
configuration.  After initialisation each call is validated and applied immediately, as a single change.

The following adds a bass shelf to a playing channel set and lowers the output trim:

<pre>
eq = obj.new('AudioEqualiser', { audio=audio, channel=channel })
check eq.mtInsertBand(0, struct.new('AudioEQBand', { type=EQB_LOW_SHELF, frequency=100, gain=4, q=0.7 }))
eq.gain = -2
</pre>

The same parameters are published through the inherited @AudioEffect schema.  The top-level keys are `gain`,
`loudness` and `listening_level`, and the `bands` group has `type`, `frequency`, `gain` and `q` members.  Use
@AudioEffect.Batch() to change several of them in a single step, e.g. a band and the output trim together:

<pre>
eq.mtBatch(array&lt;str&gt; { 'set bands[0].gain 6', 'set gain -4' })
</pre>

A band inserted by @AudioEffect.Batch() is a silent peak filter at 1000 Hz until it is edited.

Set #Loudness to apply equal-loudness compensation, which restores the bass and treble that the ear loses when
playback is quiet.  #ListeningLevel is the playback level in decibels relative to the reference level, at which
compensation is flat.  The equaliser cannot measure the acoustic playback level, so the application sets
#ListeningLevel from its own volume control.  The compensation is a low shelf and a high shelf
that approximate the difference between the ISO 226:2023 equal-loudness contours at the reference level of 80 phon
and at the listening level.  The shelves run after the bands and before the output trim, and are included in
@AudioEffect.GetResponse().  The fit error grows with attenuation; the worst error in the ISO data range of 20 Hz to
12.5 kHz is about 0.6 dB at -10 dB, 0.9 dB at -20 dB, 2 dB at -40 dB and 3.4 dB at -60 dB, where the 1 kHz level is
raised by 1.6 dB.  Compensation can boost the bass by up to 30 dB at low listening levels, so place an AudioLimiter
after the equaliser to prevent clipping.

<pre>
eq.listeningLevel = -30
eq.loudness = 1
</pre>

Live changes crossfade the old and new filter chains over 10 ms, including output trim changes.  An edit during a
crossfade replaces a single queued target, which starts its own crossfade when the current one finishes.  Parameter
reads and @AudioEffect.GetResponse() describe the latest committed target immediately, even during a transition.
Device reactivation and leaving bypass reset filter history and apply the latest target without a crossfade.

The equaliser has zero algorithmic latency but its recursive state can continue producing a decay after input
ends.  Decay remains pending until all live filter states fall below 1e-12 internal sample units, then remaining
history is discarded.  Arbitrary supported poles and live edits have no fixed finite bound, so @Audio.MaxDrain
also limits this decay.  Hard bypass and destruction can cut it abruptly.

-END-

*********************************************************************************************************************/

#include "audio_equaliser_dsp.h"

class extAudioEqualiser : public extAudioEffect {
public:
   std::vector<AudioEQBand> Bands;
   double Gain = 0;
   double ListeningLevel = -20;
   bool Loudness = false;
   EqualiserProcessor *Processor = nullptr;

   extAudioEqualiser(objMetaClass *ClassPtr, OBJECTID ObjectID);
};

//********************************************************************************************************************
// Parameter descriptors.  Member order in the band group matches the fields of AudioEQBand.

static const int glShelfTypes[] = { int(EQB::LOW_SHELF), int(EQB::HIGH_SHELF) };
static const int glPassTypes[]  = { int(EQB::LOW_PASS), int(EQB::HIGH_PASS) };

static const AudioParamOption glBandTypes[] = {
   { int(EQB::PEAK), "peak", "Peak",
     "Boosts or cuts a range of frequencies centred on the band frequency.  Q sets the width of the range." },
   { int(EQB::LOW_SHELF), "low_shelf", "Low Shelf",
     "Boosts or cuts every frequency below the band frequency by the same amount, e.g. to add or remove bass." },
   { int(EQB::HIGH_SHELF), "high_shelf", "High Shelf",
     "Boosts or cuts every frequency above the band frequency by the same amount, e.g. to add or remove treble." },
   { int(EQB::LOW_PASS), "low_pass", "Low Pass",
     "Removes frequencies above the band frequency, softening harsh or hissing sounds." },
   { int(EQB::HIGH_PASS), "high_pass", "High Pass",
     "Removes frequencies below the band frequency, reducing rumble and hum." }
};

static const int glLoudnessOff[] = { 0 };

static const AudioParamRule glBandGainRules[] = { { .Key = "type", .Values = glPassTypes, .Inactive = true } };
static const AudioParamRule glBandQRules[]    = { { .Key = "type", .Values = glShelfTypes, .CapMax = 1 } };
static const AudioParamRule glListeningLevelRules[] = {
   { .Key = "loudness", .Values = glLoudnessOff, .Inactive = true }
};

enum { BAND_TYPE = 0, BAND_FREQUENCY, BAND_GAIN, BAND_Q, BAND_MEMBERS };
enum { EQ_GAIN = 0, EQ_LOUDNESS, EQ_LISTENING_LEVEL };
enum { EQ_BANDS = 0 };

static const AudioParamDesc glBandMembers[] = {
   { .Key = "type", .Label = "Type",
     .Description = "The filter shape.  A peak boosts or cuts around the frequency, a shelf boosts or cuts everything "
        "below or above it, and a pass filter removes everything above or below it.",
     .Type = APT::ENUM, .Default = int(EQB::PEAK), .Options = glBandTypes },
   { .Key = "frequency", .Label = "Frequency",
     .Description = "The centre frequency of a peak, or the corner frequency of a shelf or pass filter.",
     .Unit = APU::HZ, .Scale = APS::LOG, .Min = 0, .Default = 1000, .MinExclusive = true, .MaxBound = APB::NYQUIST },
   { .Key = "gain", .Label = "Gain",
     .Description = "The boost (positive) or cut (negative) applied by a peak or shelf.  Pass filters ignore it.",
     .Unit = APU::DB, .Min = -48, .Max = 48, .Default = 0, .Rules = glBandGainRules },
   { .Key = "q", .Label = "Q",
     .Description = "For a peak, the width of the affected range; higher values are narrower.  For a shelf, the "
        "steepness of the transition, at most 1.  For a pass filter, the emphasis at the corner frequency.",
     .Scale = APS::LOG, .Min = 0.1, .Max = 20, .Default = 0.707, .Rules = glBandQRules }
};

static const AudioParamDesc glEqualiserParams[] = {
   { .Key = "gain", .Label = "Output Trim",
     .Description = "A level adjustment applied after all bands, typically to offset the loudness change of a boost.",
     .Unit = APU::DB, .Min = -48, .Max = 48, .Default = 0 },
   { .Key = "loudness", .Label = "Loudness",
     .Description = "Restores the bass and treble that the ear loses when playback is quiet, according to the "
        "listening level.",
     .Type = APT::BOOL, .Min = 0, .Max = 1, .Default = 0 },
   { .Key = "listening_level", .Label = "Listening Level",
     .Description = "The playback level relative to the reference level, at which loudness compensation is flat.  "
        "Set it from the application's volume control.  Lower levels receive more bass and treble.",
     .Unit = APU::DB, .Min = -60, .Max = 0, .Default = -20, .Rules = glListeningLevelRules }
};

static const AudioParamGroup glEqualiserGroups[] = {
   { .Key = "bands", .Label = "Band",
     .Description = "Filters applied one after another, in list order.  A new band has no effect until it is edited.",
     .MinCount = 0, .MaxCount = 64, .Members = glBandMembers }
};

static const AudioOutputDesc glEqualiserOutputs[] = {
   { "response" },
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", "Maximum input sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", "Maximum output sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::OUTPUT_PEAK }
};

//********************************************************************************************************************

static AudioEQBand entry_band(const AudioParamEntry &Entry)
{
   return AudioEQBand { EQB(int(Entry.Values[BAND_TYPE])), Entry.Values[BAND_FREQUENCY], Entry.Values[BAND_GAIN],
      Entry.Values[BAND_Q] };
}

static std::array<double, BAND_MEMBERS> band_values(const AudioEQBand &Band)
{
   return { double(int(Band.Type)), Band.Frequency, Band.Gain, Band.Q };
}

//********************************************************************************************************************

static void equaliser_read(extAudioEffect *Effect, AudioParamState &State)
{
   auto Self = (extAudioEqualiser *)Effect;
   State.Params.assign({ Self->Gain, double(Self->Loudness), Self->ListeningLevel });
   State.Groups.resize(1);
   auto &entries = State.Groups[EQ_BANDS];
   entries.clear();
   entries.reserve(Self->Bands.size());
   for (const auto &band : Self->Bands) {
      entries.push_back({ { double(int(band.Type)), band.Frequency, band.Gain, band.Q } });
   }
}

//********************************************************************************************************************
// Before initialisation there is no processor and individual edits need not satisfy cross-parameter rules.

static void equaliser_apply(extAudioEffect *Effect, const AudioParamState &State)
{
   auto Self = (extAudioEqualiser *)Effect;
   Self->Bands.clear();
   for (const auto &entry : State.Groups[EQ_BANDS]) Self->Bands.push_back(entry_band(entry));
   Self->Gain = State.Params[EQ_GAIN];
   Self->Loudness = State.Params[EQ_LOUDNESS] != 0;
   Self->ListeningLevel = State.Params[EQ_LISTENING_LEVEL];
}

//********************************************************************************************************************
// Prepared storage is also the retirement container: swaps leave old allocations here to be freed after unlocking.
// Compensation shelves follow the bands in the section list.  Their origins are resolved on publication so that a
// listening level edit keeps the history of the committed shelves.

class EqualiserUpdate final : public AudioParamUpdate {
public:
   std::vector<AudioEQBand> Bands;
   std::vector<EQSection> Sections;
   std::vector<int> Origins;
   double Gain, Trim, ListeningLevel;
   bool Loudness;

   EqualiserUpdate(extAudioEffect *Effect, const AudioParamState &State, int Rate)
      : Gain(State.Params[EQ_GAIN]), Trim(std::pow(10.0, Gain / 20.0)),
        ListeningLevel(State.Params[EQ_LISTENING_LEVEL]), Loudness(State.Params[EQ_LOUDNESS] != 0) {
      const auto &entries = State.Groups[EQ_BANDS];
      Bands.reserve(entries.size());
      Origins.reserve(entries.size() + 2);
      for (const auto &entry : entries) {
         Bands.push_back(entry_band(entry));
         Origins.push_back(entry.Origin);
      }
      if (Loudness) Origins.insert(Origins.end(), 2, -1);
      EqualiserProcessor model(Effect, equaliser_sections(Bands, Loudness, ListeningLevel), Gain);
      model.Rate = Rate;
      for (auto &section : model.Sections) model.compute(section);
      Sections.swap(model.Sections);
   }

   void publish(extAudioEffect *Effect) override {
      auto Self = (extAudioEqualiser *)Effect;
      if (Loudness and Self->Loudness) {
         Origins[Bands.size()] = int(Self->Bands.size());
         Origins[Bands.size() + 1] = int(Self->Bands.size()) + 1;
      }
      if (Self->Processor) Self->Processor->update(Sections, Origins, Trim);
      Self->Bands.swap(Bands);
      Self->Gain = Gain;
      Self->Loudness = Loudness;
      Self->ListeningLevel = ListeningLevel;
   }
};

static std::unique_ptr<AudioParamUpdate> equaliser_prepare(extAudioEffect *Effect, const AudioParamState &State,
   int Rate)
{
   return std::make_unique<EqualiserUpdate>(Effect, State, Rate);
}

static size_t equaliser_group_count(extAudioEffect *Effect, size_t Group)
{
   return ((extAudioEqualiser *)Effect)->Bands.size();
}

//********************************************************************************************************************
// Evaluates |H(e^jw)| for every band at the current output rate.  Coefficients are computed from the committed bands
// rather than read from the processor, so no mixer lock is needed for the section data.

static ERR equaliser_response(extAudioEffect *Effect, std::span<const double> Frequencies,
   std::span<double> Magnitudes)
{
   auto Self = (extAudioEqualiser *)Effect;
   EqualiserProcessor model(Self, equaliser_sections(Self->Bands, Self->Loudness, Self->ListeningLevel), Self->Gain);
   model.Rate = effect_rate(Self);
   for (auto &section : model.Sections) model.compute(section);
   equaliser_magnitudes(model, Frequencies, Magnitudes);
   return ERR::Okay;
}

//********************************************************************************************************************

static const AudioEffectSchema glEqualiserSchema = {
   .ClassName   = "AudioEqualiser",
   .Version     = 2,
   .Description = "A parametric equaliser that shapes the tone of the sound with a list of filter bands.",
   .Params      = glEqualiserParams,
   .Groups      = glEqualiserGroups,
   .Outputs     = glEqualiserOutputs,
   .Read        = equaliser_read,
   .Apply       = equaliser_apply,
   .Response    = equaliser_response,
   .Prepare     = equaliser_prepare,
   .GroupCount  = equaliser_group_count
};

//********************************************************************************************************************

extAudioEqualiser::extAudioEqualiser(objMetaClass *ClassPtr, OBJECTID ObjectID) : extAudioEffect(ClassPtr, ObjectID)
{
   Schema = &glEqualiserSchema;
}

//********************************************************************************************************************

static ERR AUDIOEQUALISER_Init(extAudioEqualiser *Self)
{
   if (Self->Processor) return ERR::InvalidState;
   auto chain = Self->Chain.lock();
   if (!chain) return ERR::NotInitialised;
   std::lock_guard mixer_lock(*chain->Mutex);

   AudioParamState state;
   equaliser_read(Self, state);
   if (validate_state(glEqualiserSchema, state, Self->OutputRate) != ERR::Okay) return ERR::InvalidValue;

   auto processor = std::make_unique<EqualiserProcessor>(Self,
      equaliser_sections(Self->Bands, Self->Loudness, Self->ListeningLevel), Self->Gain);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;

   return error;
}

/*********************************************************************************************************************

-METHOD-
GetBand: Reads one band.

Copies the committed values of the band at `Index` to `Band`.  Bands can be read before initialisation, from a global
equaliser, and after the equaliser has been disconnected.

<pre>
band = struct.new('AudioEQBand')
check eq.mtGetBand(0, band)
</pre>

-INPUT-
int Index: The zero-based index of the band.
struct(*AudioEQBand) Band: Receives the band's type, frequency, gain and Q.

-ERRORS-
Okay
NullArgs
OutOfRange: `Index` does not refer to an existing band.
-END-

*********************************************************************************************************************/

static ERR AUDIOEQUALISER_GetBand(extAudioEqualiser *Self, struct eq::GetBand *Args)
{
   if ((not Args) or (not Args->Band)) return ERR::NullArgs;
   if ((Args->Index < 0) or (Args->Index >= std::ssize(Self->Bands))) return ERR::OutOfRange;
   *Args->Band = Self->Bands[Args->Index];
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
InsertBand: Inserts a band with the given values.

Inserts `Band` at `Index`, moving the band at that position and every later band up by one.  The new band starts with
fresh filter history, while the other bands keep theirs, so a live insertion does not disturb them.

-INPUT-
int Index: The position of the new band, from zero to the current band count.
struct(*AudioEQBand) Band: The type, frequency, gain and Q of the new band.

-ERRORS-
Okay
NullArgs
OutOfRange: `Index` is greater than the band count, or the equaliser already has 64 bands.
InvalidValue: A value is out of range, or Q exceeds one for a shelf after initialisation.
Immutable: The equaliser is attached to the global chain.
NotInitialised: The equaliser has been disconnected from its Audio object.
-END-

*********************************************************************************************************************/

static ERR AUDIOEQUALISER_InsertBand(extAudioEqualiser *Self, struct eq::InsertBand *Args)
{
   if ((not Args) or (not Args->Band)) return ERR::NullArgs;
   return effect_group_insert(Self, EQ_BANDS, Args->Index, band_values(*Args->Band));
}

/*********************************************************************************************************************

-METHOD-
RemoveBand: Removes one band.

Removes the band at `Index`, moving every later band down by one.  The remaining bands keep their filter history.

-INPUT-
int Index: The zero-based index of the band.

-ERRORS-
Okay
NullArgs
OutOfRange: `Index` does not refer to an existing band.
Immutable: The equaliser is attached to the global chain.
NotInitialised: The equaliser has been disconnected from its Audio object.
-END-

*********************************************************************************************************************/

static ERR AUDIOEQUALISER_RemoveBand(extAudioEqualiser *Self, struct eq::RemoveBand *Args)
{
   if (not Args) return ERR::NullArgs;
   return effect_group_remove(Self, EQ_BANDS, Args->Index);
}

/*********************************************************************************************************************

-METHOD-
SetBand: Replaces the values of one band.

Replaces the type, frequency, gain and Q of the band at `Index` as a single change.  The band keeps its filter history
and a live edit crossfades to the new response.  Because every value changes together, a peak can be changed to a
shelf and its Q reduced to one in the same call.

<pre>
check eq.mtSetBand(0, struct.new('AudioEQBand', { type=EQB_HIGH_SHELF, frequency=8000, gain=-3, q=0.5 }))
</pre>

-INPUT-
int Index: The zero-based index of the band.
struct(*AudioEQBand) Band: The new type, frequency, gain and Q.

-ERRORS-
Okay
NullArgs
OutOfRange: `Index` does not refer to an existing band.
InvalidValue: A value is out of range, or Q exceeds one for a shelf after initialisation.
Immutable: The equaliser is attached to the global chain.
NotInitialised: The equaliser has been disconnected from its Audio object.
-END-

*********************************************************************************************************************/

static ERR AUDIOEQUALISER_SetBand(extAudioEqualiser *Self, struct eq::SetBand *Args)
{
   if ((not Args) or (not Args->Band)) return ERR::NullArgs;
   return effect_group_set(Self, EQ_BANDS, Args->Index, band_values(*Args->Band));
}

/*********************************************************************************************************************

-FIELD-
Bands: The ordered array of AudioEQBand structures.

The list can contain at most 64 bands.  Replacing the list clears the filter history of every band, so use #SetBand(),
#InsertBand() and #RemoveBand() for live adjustment; they preserve the history of the bands that they do not
replace.  A global equaliser accepts this field only before initialisation.

*********************************************************************************************************************/

static ERR AUDIOEQUALISER_GET_Bands(extAudioEqualiser *Self, std::span<AudioEQBand> &Value)
{
   Value = std::span<AudioEQBand>(Self->Bands.data(), Self->Bands.size());
   return ERR::Okay;
}

static ERR AUDIOEQUALISER_SET_Bands(extAudioEqualiser *Self, std::span<const AudioEQBand> &Value)
{
   if (Value.size() > 64) return ERR::Args;

   AudioParamState state;
   equaliser_read(Self, state);
   state.Groups[EQ_BANDS].clear();
   for (const auto &band : Value) { // Origins remain -1 so that every band is validated and starts afresh
      state.Groups[EQ_BANDS].push_back({ { double(int(band.Type)), band.Frequency, band.Gain, band.Q } });
   }
   return effect_set_state(Self, state);
}

/*********************************************************************************************************************

-FIELD-
Gain: Output trim in decibels, applied after all bands.

The range is -48 to 48 dB and the default is 0 dB.  This field mirrors the `gain` schema key.  To change it together
with other parameters in a single step, use @AudioEffect.Batch().

*********************************************************************************************************************/

static ERR AUDIOEQUALISER_GET_Gain(extAudioEqualiser *Self, double *Value)
{
   *Value = Self->Gain;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ListeningLevel: The playback level, in decibels relative to the reference level, used for loudness compensation.

The range is -60 to 0 dB and the default is -20 dB.  Set it from the application's volume control; lower levels
receive more bass and treble, and compensation is flat at 0 dB.  The value has no effect unless #Loudness is enabled.
This field mirrors the `listening_level` schema key.  To change it together with other parameters in a single step,
use @AudioEffect.Batch().

*********************************************************************************************************************/

static ERR AUDIOEQUALISER_GET_ListeningLevel(extAudioEqualiser *Self, double *Value)
{
   *Value = Self->ListeningLevel;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Loudness: Enables equal-loudness compensation.

Set to 1 to restore the bass and treble that the ear loses when playback is quiet, according to #ListeningLevel.  The
default is 0 (disabled).  This field mirrors the `loudness` schema key.  To change it together with other parameters
in a single step, use @AudioEffect.Batch().

*********************************************************************************************************************/

static ERR AUDIOEQUALISER_GET_Loudness(extAudioEqualiser *Self, int *Value)
{
   *Value = Self->Loudness ? 1 : 0;
   return ERR::Okay;
}

//********************************************************************************************************************

#include "class_audioequaliser_def.c"

static const FieldArray clAudioEqualiserFields[] = {
   { "Bands", FDF_VIRTUAL|FDF_ARRAY|FDF_STRUCT|FDF_RW|FDF_PURE, AUDIOEQUALISER_GET_Bands, AUDIOEQUALISER_SET_Bands, "AudioEQBand" },
   { "Gain",  FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, AUDIOEQUALISER_GET_Gain, effect_param_set<EQ_GAIN> },
   { "Loudness", FDF_VIRTUAL|FDF_INT|FDF_RW|FDF_PURE, AUDIOEQUALISER_GET_Loudness, effect_param_set_int<EQ_LOUDNESS> },
   { "ListeningLevel", FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE,
     AUDIOEQUALISER_GET_ListeningLevel, effect_param_set<EQ_LISTENING_LEVEL> },
   END_FIELD
};

//********************************************************************************************************************

static ERR add_audioequaliser_class()
{
   clAudioEqualiser = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIOEFFECT),
      fl::ClassID(CLASSID::AUDIOEQUALISER),
      fl::Name("AudioEqualiser"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioEqualiserActions),
      fl::Methods(clAudioEqualiserMethods),
      fl::Fields(clAudioEqualiserFields),
      fl::Size(sizeof(extAudioEqualiser)),
      fl::Path(MOD_PATH));
   return clAudioEqualiser ? ERR::Okay : ERR::AddClass;
}
