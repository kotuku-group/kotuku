/*********************************************************************************************************************

-CLASS-
AudioEqualiser: A parametric equaliser for an Audio mixing chain.

Create as a child of an Audio object or set the inherited Audio field.  Use Channel to process one channel set, or
leave it at zero to process the global mix.  Bands run in list order.  Application equalisers accept live changes;
global equalisers become immutable after initialisation.

The equaliser publishes its parameters through the inherited @AudioEffect schema.  The top-level `gain` parameter is
the output trim.  The `bands` group holds up to 64 bands, each with `type`, `frequency`, `gain` and `q` members.  Band
gain is ignored by pass filters, and Q is limited to one for shelf filters, where it sets the shelf slope.  New bands
are silent peak filters at 1000 Hz until they are edited.

The following adds a bass shelf to a playing channel set.  The change is applied as a single step by @AudioEffect.Flush():

<pre>
eq.mtInsertEntry('bands', 0)
eq.mtSetParameter('bands[0].type', EQB_LOW_SHELF)
eq.mtSetParameter('bands[0].frequency', 100)
eq.mtSetParameter('bands[0].gain', 4)
eq.mtSetParameter('bands[0].q', 0.7)
eq.acFlush()
</pre>

Assign the #Bands field to replace every band at once.

Live changes crossfade the old and new filter chains over 10 ms, including output trim changes.  An edit during a
crossfade replaces a single queued target, which starts its own crossfade when the current one finishes.  Parameter
reads and @AudioEffect.GetResponse() describe the latest committed target immediately, even during a transition.
Device reactivation and leaving bypass reset filter history and apply the latest target without a crossfade.

-END-

*********************************************************************************************************************/

#include "audio_equaliser_dsp.h"

class extAudioEqualiser : public extAudioEffect {
public:
   std::vector<AudioEQBand> Bands;
   double Gain = 0;
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

static const AudioParamRule glBandGainRules[] = { { .Key = "type", .Values = glPassTypes, .Inactive = true } };
static const AudioParamRule glBandQRules[]    = { { .Key = "type", .Values = glShelfTypes, .CapMax = 1 } };

enum { BAND_TYPE = 0, BAND_FREQUENCY, BAND_GAIN, BAND_Q, BAND_MEMBERS };
enum { EQ_GAIN = 0 };
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
     .Unit = APU::DB, .Min = -48, .Max = 48, .Default = 0 }
};

static const AudioParamGroup glEqualiserGroups[] = {
   { .Key = "bands", .Label = "Band",
     .Description = "Filters applied one after another, in list order.  A new band has no effect until it is edited.",
     .MinCount = 0, .MaxCount = 64, .Members = glBandMembers }
};

static const CSTRING glEqualiserOutputs[] = { "response" };

//********************************************************************************************************************

static AudioEQBand entry_band(const AudioParamEntry &Entry)
{
   return AudioEQBand { EQB(int(Entry.Values[BAND_TYPE])), Entry.Values[BAND_FREQUENCY], Entry.Values[BAND_GAIN],
      Entry.Values[BAND_Q] };
}

static void equaliser_read(extAudioEffect *Effect, AudioParamState &State)
{
   auto Self = (extAudioEqualiser *)Effect;
   State.Params.assign({ Self->Gain });
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
}

// Prepared storage is also the retirement container: swaps leave old allocations here to be freed after unlocking.

class EqualiserUpdate final : public AudioParamUpdate {
public:
   std::vector<AudioEQBand> Bands;
   std::vector<EQSection> Sections;
   std::vector<int> Origins;
   double Gain, Trim;

   EqualiserUpdate(extAudioEffect *Effect, const AudioParamState &State, int Rate)
      : Gain(State.Params[EQ_GAIN]), Trim(std::pow(10.0, Gain / 20.0)) {
      const auto &entries = State.Groups[EQ_BANDS];
      Bands.reserve(entries.size());
      Origins.reserve(entries.size());
      for (const auto &entry : entries) {
         Bands.push_back(entry_band(entry));
         Origins.push_back(entry.Origin);
      }
      EqualiserProcessor model(Effect, Bands, Gain);
      model.Rate = Rate;
      for (auto &section : model.Sections) model.compute(section);
      Sections.swap(model.Sections);
   }

   void publish(extAudioEffect *Effect) override {
      auto Self = (extAudioEqualiser *)Effect;
      if (Self->Processor) Self->Processor->update(Sections, Origins, Trim);
      Self->Bands.swap(Bands);
      Self->Gain = Gain;
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
   EqualiserProcessor model(Self, Self->Bands, Self->Gain);
   model.Rate = effect_rate(Self);
   for (auto &section : model.Sections) model.compute(section);
   equaliser_magnitudes(model, Frequencies, Magnitudes);
   return ERR::Okay;
}

//********************************************************************************************************************

static const AudioEffectSchema glEqualiserSchema = {
   .ClassName   = "AudioEqualiser",
   .Version     = 1,
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

   auto processor = std::make_unique<EqualiserProcessor>(Self, Self->Bands, Self->Gain);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;

   return error;
}

/*********************************************************************************************************************

-FIELD-
Bands: The ordered array of AudioEQBand structures.

The list can contain at most 64 bands.  Replacing the list clears filter history, so use @AudioEffect.SetParameter()
for live adjustment.  A global equaliser accepts this field only before initialisation.  Writes return
`ERR::InvalidState` while changes staged by @AudioEffect.SetParameter() are waiting for @AudioEffect.Flush().

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
   state.Params.assign({ Self->Gain });
   state.Groups.resize(1);
   for (const auto &band : Value) { // Origins remain -1 so that every band is validated and starts afresh
      state.Groups[EQ_BANDS].push_back({ { double(int(band.Type)), band.Frequency, band.Gain, band.Q } });
   }
   return effect_set_state(Self, state);
}

/*********************************************************************************************************************

-FIELD-
Gain: Output trim in decibels, applied after all bands.

Writes return `ERR::InvalidState` while changes staged by @AudioEffect.SetParameter() are waiting for @AudioEffect.Flush().

*********************************************************************************************************************/

static ERR AUDIOEQUALISER_GET_Gain(extAudioEqualiser *Self, double *Value)
{
   *Value = Self->Gain;
   return ERR::Okay;
}

static ERR AUDIOEQUALISER_SET_Gain(extAudioEqualiser *Self, double Value)
{
   AudioParamState state;
   effect_snapshot(Self, state);
   state.Params[EQ_GAIN] = Value;
   return effect_set_state(Self, state);
}

//********************************************************************************************************************

#include "class_audioequaliser_def.c"

static const FieldArray clAudioEqualiserFields[] = {
   { "Bands", FDF_VIRTUAL|FDF_ARRAY|FDF_STRUCT|FDF_RW|FDF_PURE, AUDIOEQUALISER_GET_Bands, AUDIOEQUALISER_SET_Bands, "AudioEQBand" },
   { "Gain",  FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, AUDIOEQUALISER_GET_Gain, AUDIOEQUALISER_SET_Gain },
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
      fl::Fields(clAudioEqualiserFields),
      fl::Size(sizeof(extAudioEqualiser)),
      fl::Path(MOD_PATH));
   return clAudioEqualiser ? ERR::Okay : ERR::AddClass;
}
