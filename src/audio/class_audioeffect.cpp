/*********************************************************************************************************************

-CLASS-
AudioEffect: Support class for processors attached to an Audio mixing chain.

AudioEffect is a support class for audio processors.  Use a derived class to perform DSP; the AudioEffect class
itself passes samples through unchanged.

A zero #Channel applies the effect universally.  Otherwise use the complete handle returned by @Audio.OpenChannels().
Application chains process their channel set before it is added to the master mix.  The global chain processes the
master mix before the built-in output filter.  Lower #Order values run first; ties retain attachment order.

Attachment, field changes and detachment are serialised with mixing.  Freeing an effect waits for any current mix
window before detaching it.  Closing its channel set or freeing its Audio target disconnects the effect; a
disconnected effect must be replaced to attach it again.  Global effects are immutable after initialisation.

<header>Parameter Schema</header>

Subclasses can publish their parameters in the #Schema field, so that a client can present and change any effect
without knowing its class.  The schema is an XML document:

<pre>
&lt;effect class="AudioEqualiser" version="1" description="A parametric equaliser."&gt;
  &lt;param key="gain" label="Output Trim" type="double" unit="dB" scale="linear" min="-48" max="48" default="0"/&gt;
  &lt;group key="bands" label="Band" min="0" max="64"&gt;
    &lt;param key="type" label="Type" type="enum" default="0"&gt;
      &lt;option value="0" key="peak" label="Peak" description="Boosts or cuts a range."/&gt;
    &lt;/param&gt;
    &lt;param key="q" label="Q" type="double" scale="log" min="0.1" max="20" default="0.707"&gt;
      &lt;rule key="type" values="1,2" max="1"/&gt;
    &lt;/param&gt;
  &lt;/group&gt;
  &lt;output key="response" type="curve"/&gt;
&lt;/effect&gt;
</pre>

The `effect`, `group`, `param` and `option` elements may carry a `description` attribute.  It contains one or more
complete sentences that explain the item's purpose in terms suitable for presentation to the user, e.g. as a tooltip.
The attribute is omitted if no description is available.

A `param` element describes one value.  Its `type` is `double`, `int`, `bool` or `enum`; enumerations list their
values as `option` elements.  The `unit` attribute is omitted for unitless values.  The `scale` is `linear` or `log`
and indicates how a control should map positions to values.  The `min` and `max` bounds are inclusive unless
`min-exclusive` or `max-exclusive` is set.  A `max` of `nyquist` means half of the #OutputRate and is always exclusive.
A `step` is present only for stepped values.

A `group` element describes a repeated structure, such as a list of equaliser bands, with `min` and `max` entry counts.
Its entries are numbered from zero.
Use #GetGroupCount() to read the number of entries, including staged insertions and removals.

A `rule` element relates a parameter to a sibling in the same group, or to another top-level parameter.  If the
sibling named by `key` holds one of the listed `values`, the parameter is either ignored by the processor
(`inactive="1"`) or limited to a lower `max`.

An `output` element names data that can be read back from the processor.  A `curve` output is read with
#GetResponse().

Parameters are addressed by key-paths: `key` for a top-level parameter, or `group[index].key` for a group member, e.g.
`bands[2].frequency`.  Values are always in real units, such as hertz or decibels; they are never normalised.

<header>Staged Changes</header>

Before initialisation, #SetParameter(), #InsertEntry() and #RemoveEntry() change the effect directly and `Init()`
validates the result.  After initialisation they change a working copy instead, which is applied as a single change
when the client calls `Flush()`.  This allows related changes, such as switching a band's type together with its Q,
without the processor ever running the state between them.  #GetParameter() reports staged values.  A flush that
breaks any rule is rejected in full and the working copy is discarded.

-END-

*********************************************************************************************************************/

//********************************************************************************************************************
// Returns an error if the effect's parameters cannot currently be changed.

static ERR effect_mutable(extAudioEffect *Self)
{
   if (Self->initialised() and !Self->Channel) return ERR::Immutable;
   if (Self->initialised() and Self->Chain.expired()) return ERR::NotInitialised;
   return ERR::Okay;
}

//********************************************************************************************************************
// OutputRate is updated by device activation under the mixer lock.

static int effect_rate(extAudioEffect *Self)
{
   if (auto chain = Self->Chain.lock()) {
      std::lock_guard mixer_lock(*chain->Mutex);
      return Self->OutputRate;
   }
   return Self->OutputRate;
}

//********************************************************************************************************************
// Snapshot the committed parameters.  Every entry records its own index as its origin.

static void effect_snapshot(extAudioEffect *Self, AudioParamState &State)
{
   Self->Schema->Read(Self, State);
   for (auto &entries : State.Groups) {
      for (size_t i = 0; i < entries.size(); i++) entries[i].Origin = int(i);
   }
}

//********************************************************************************************************************
// Prepare storage and coefficients outside the mixer lock.  Recheck the rate before publication because activation
// can negotiate a different rate during preparation.  Prepared updates retire their old storage after unlocking.

static ERR effect_commit(extAudioEffect *Self, const AudioParamState &State)
{
   AudioParamState committed;
   effect_snapshot(Self, committed);

   auto chain = Self->Chain.lock();
   for (;;) {
      const int rate = effect_rate(Self);
      if (validate_state(*Self->Schema, State, rate, &committed) != ERR::Okay) {
         return ERR::InvalidValue;
      }
      auto update = Self->Schema->Prepare ? Self->Schema->Prepare(Self, State, rate) : nullptr;
      {
         std::unique_lock<std::recursive_mutex> mixer_lock;
         if (chain) mixer_lock = std::unique_lock(*chain->Mutex);
         if (Self->OutputRate != rate) continue;
         if (update) update->publish(Self);
         else Self->Schema->Apply(Self, State);
      }
      return ERR::Okay;
   }
}

//********************************************************************************************************************
// Apply an edit to the working copy after initialisation, or directly to the effect before initialisation.  Before
// initialisation only each value's own range is checked; Init() validates the complete state.

template <class T> static ERR effect_edit(extAudioEffect *Self, T &&Edit)
{
   if (not Self->Schema) return ERR::NoSupport;
   if (auto error = effect_mutable(Self); error != ERR::Okay) return error;

   if (Self->initialised()) {
      const bool created = not Self->Pending;
      if (created) {
         Self->Pending = std::make_unique<AudioParamState>();
         effect_snapshot(Self, *Self->Pending);
      }
      auto error = Edit(*Self->Pending);
      if ((error != ERR::Okay) and created) Self->Pending.reset();
      return error;
   }

   AudioParamState state;
   effect_snapshot(Self, state);
   if (auto error = Edit(state); error != ERR::Okay) return error;
   Self->Schema->Apply(Self, state);
   return ERR::Okay;
}

//********************************************************************************************************************
// Used by subclass field setters that commit immediately.

static ERR effect_set_state(extAudioEffect *Self, const AudioParamState &State)
{
   if (auto error = effect_mutable(Self); error != ERR::Okay) return error;
   if (Self->Pending) return ERR::InvalidState;
   return effect_commit(Self, State);
}

/*********************************************************************************************************************

-ACTION-
Flush: Applies changes staged by SetParameter(), InsertEntry() and RemoveEntry().

After initialisation, parameter changes made through #SetParameter(), #InsertEntry() and #RemoveEntry() are held in a
working copy.  Flush() validates the working copy as a whole and applies it in a single step, so the processor never
runs a partially changed state.

The working copy is discarded whether or not the flush succeeds.  If any value or rule is invalid, the effect is left
unchanged and `ERR::InvalidValue` is returned.  Calling Flush() with no staged changes has no effect.

-ERRORS-
Okay: The staged changes were applied, or there were none.
InvalidValue: The staged changes break a rule or range.  Nothing was applied.
Immutable: The effect is attached to the global chain.
NotInitialised: The effect has been disconnected from its Audio object.
-END-

*********************************************************************************************************************/

static ERR AUDIOEFFECT_Flush(extAudioEffect *Self)
{
   if (not Self->Pending) return ERR::Okay;
   auto state = std::move(Self->Pending);
   if (auto error = effect_mutable(Self); error != ERR::Okay) return error;
   return effect_commit(Self, *state);
}

//********************************************************************************************************************
// Detach before a derived destructor starts destroying processor state.

static ERR AUDIOEFFECT_FreeWarning(extAudioEffect *Self)
{
   Self->detach();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR AUDIOEFFECT_NewOwner(extAudioEffect *Self, struct acNewOwner *Args)
{
   if (!Args) return ERR::NullArgs;
   if (!Self->initialised() and !Self->AudioID and Args->NewOwner and
       (Args->NewOwner->Class->BaseClassID IS CLASSID::AUDIO)) {
      Self->AudioID = Args->NewOwner->UID;
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR AUDIOEFFECT_Init(extAudioEffect *Self)
{
   // A failed derived Init may be retried; never register the same object twice.

   Self->detach();
   if (!Self->AudioID and Self->Owner and (Self->Owner->Class->BaseClassID IS CLASSID::AUDIO)) {
      Self->AudioID = Self->Owner->UID;
   }
   if (!Self->AudioID) return ERR::FieldNotSet;
   if ((Self->Flags & ~AEF::BYPASS) != AEF::NIL) return ERR::InvalidValue;
   if (!Self->Channel and (Self->Flags != AEF::NIL)) return ERR::InvalidValue;

   kt::ScopedObjectLock<extAudio> audio(Self->AudioID, 3000);
   if (!audio.granted()) return ERR::Search;
   if (audio->Class->BaseClassID != CLASSID::AUDIO) return ERR::InvalidObject;
   if (!audio->initialised()) return ERR::NotInitialised;

   std::lock_guard mixer_lock(audio->MixerMutex);
   auto chain = audio->GlobalEffects;
   if (Self->Channel) {
      const int index = Self->Channel >> 16;
      if ((Self->Channel < 0) or (Self->Channel & 0xffff) or (index < 1) or
          (index >= std::ssize(audio->Sets)) or audio->Sets[index].Channel.empty()) return ERR::Args;
      auto &set = audio->Sets[index];
      if (!set.Effects) set.Effects = std::make_shared<AudioEffectChain>(audio->MixerLock);
      set.ScratchBuffer.resize(audio->MixBuffer.size());
      chain = set.Effects;
   }

   Self->OutputRate   = audio->OutputRate;
   Self->Stereo       = audio->MixBuffer.empty() ? ((audio->Flags & ADF::STEREO) != ADF::NIL) : audio->Stereo;
   Self->ResetPending = true;
   Self->Sequence     = chain->NextSequence++;
   Self->Chain        = chain;
   chain->Effects.push_back(Self);
   sort_effects(*chain);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetParameter: Reads a parameter value by key.

Reads the value of a parameter published in the #Schema.  If changes are staged, the staged value is returned, so a
client always reads back what it has set.  Enumerated values are returned as their numeric value.

-INPUT-
strview Path: A parameter key such as `gain` or `bands[2].frequency`.
&double Value: The parameter value is returned here.

-ERRORS-
Okay
NullArgs
NoSupport: The effect does not publish a schema.
Search: The key is malformed or names an unknown parameter.
OutOfRange: The group index does not refer to an existing entry.
-END-

*********************************************************************************************************************/

static ERR AUDIOEFFECT_GetParameter(extAudioEffect *Self, struct fx::GetParameter *Args)
{
   if (not Args) return ERR::NullArgs;
   if (not Self->Schema) return ERR::NoSupport;

   AudioParamState committed;
   auto state = Self->Pending.get();
   if (not state) {
      effect_snapshot(Self, committed);
      state = &committed;
   }

   double *value;
   const AudioParamDesc *desc;
   if (auto error = resolve_path(*Self->Schema, *state, Args->Path, value, desc); error != ERR::Okay) return error;
   Args->Value = *value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetGroupCount: Reads the number of entries in a parameter group.

Returns the current entry count for the named group.  Staged insertions and removals are included, matching
#GetParameter().  A failed Flush() discards staged changes, so subsequent queries return the committed count.
The count can be read before initialisation or after the effect has been disconnected.

-INPUT-
strview Group: The group key published in #Schema, e.g. `bands`.
&int Count: The number of entries is returned here.

-ERRORS-
Okay
NullArgs
NoSupport: The effect does not publish a schema.
Search: The group is unknown.
-END-

*********************************************************************************************************************/

static ERR AUDIOEFFECT_GetGroupCount(extAudioEffect *Self, struct fx::GetGroupCount *Args)
{
   if (not Args) return ERR::NullArgs;
   Args->Count = 0;
   if (not Self->Schema) return ERR::NoSupport;
   const auto group = find_group(*Self->Schema, Args->Group);
   if (group < 0) return ERR::Search;
   if (Self->Pending) Args->Count = int(Self->Pending->Groups[group].size());
   else if (Self->Schema->GroupCount) Args->Count = int(Self->Schema->GroupCount(Self, group));
   else {
      AudioParamState state;
      Self->Schema->Read(Self, state);
      Args->Count = int(state.Groups[group].size());
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetResponse: Computes the effect's frequency response.

Computes the response of the effect at each of the given frequencies, in decibels, from its committed parameters at
the current #OutputRate.  Staged changes are not included.  Effects that support this method list an `output` of
type `curve` in their #Schema.

-INPUT-
array(double) Frequencies: Frequencies in hertz.  Each must be above zero and below half of the #OutputRate.
^array(double) Magnitudes: Receives the response in decibels.  Must be the same length as `Frequencies`.

-ERRORS-
Okay
NullArgs
Args: The arrays are empty, of different lengths, or longer than 4096 entries.
InvalidValue: A frequency is not above zero and below half of the output rate.
NotInitialised: The output rate is not yet known.
NoSupport: The effect does not compute a response.
-END-

*********************************************************************************************************************/

static ERR AUDIOEFFECT_GetResponse(extAudioEffect *Self, struct fx::GetResponse *Args)
{
   if (not Args) return ERR::NullArgs;
   if ((not Self->Schema) or (not Self->Schema->Response)) return ERR::NoSupport;
   if (Args->Frequencies.empty() or (Args->Frequencies.size() != Args->Magnitudes.size())) return ERR::Args;
   if (Args->Frequencies.size() > 4096) return ERR::Args;

   const int rate = effect_rate(Self);
   if (rate < 2) return ERR::NotInitialised;
   for (auto frequency : Args->Frequencies) {
      if ((not std::isfinite(frequency)) or (frequency <= 0) or (frequency >= double(rate) * 0.5)) {
         return ERR::InvalidValue;
      }
   }
   return Self->Schema->Response(Self, Args->Frequencies, Args->Magnitudes);
}

/*********************************************************************************************************************

-METHOD-
InsertEntry: Inserts a new entry into a parameter group.

Inserts an entry at `Index`, filled with the default values published in the #Schema.  Later entries move up by one.
Set `Index` to the current entry count to append.  After initialisation the insertion is staged until `Flush()` is
called.

-INPUT-
strview Group: The key of the group, e.g. `bands`.
int Index: The zero-based position for the new entry.

-ERRORS-
Okay
NullArgs
NoSupport: The effect does not publish a schema.
Search: The group is unknown.
OutOfRange: The index is invalid or the group is full.
Immutable: The effect is attached to the global chain.
NotInitialised: The effect has been disconnected from its Audio object.
-END-

*********************************************************************************************************************/

static ERR AUDIOEFFECT_InsertEntry(extAudioEffect *Self, struct fx::InsertEntry *Args)
{
   if (not Args) return ERR::NullArgs;
   const int rate = effect_rate(Self);

   return effect_edit(Self, [&](AudioParamState &State) {
      auto g = find_group(*Self->Schema, Args->Group);
      if (g < 0) return ERR::Search;
      const auto &group = Self->Schema->Groups[g];
      auto &entries = State.Groups[g];
      if ((Args->Index < 0) or (size_t(Args->Index) > entries.size())) return ERR::OutOfRange;
      if (int(entries.size()) >= group.MaxCount) return ERR::OutOfRange;
      entries.insert(entries.begin() + Args->Index, default_entry(group, rate));
      return ERR::Okay;
   });
}

/*********************************************************************************************************************

-METHOD-
RemoveEntry: Removes an entry from a parameter group.

Removes the entry at `Index`.  Later entries move down by one and keep their processing state.  After initialisation
the removal is staged until `Flush()` is called.

-INPUT-
strview Group: The key of the group, e.g. `bands`.
int Index: The zero-based position of the entry to remove.

-ERRORS-
Okay
NullArgs
NoSupport: The effect does not publish a schema.
Search: The group is unknown.
OutOfRange: The index is invalid or the group is at its minimum size.
Immutable: The effect is attached to the global chain.
NotInitialised: The effect has been disconnected from its Audio object.
-END-

*********************************************************************************************************************/

static ERR AUDIOEFFECT_RemoveEntry(extAudioEffect *Self, struct fx::RemoveEntry *Args)
{
   if (not Args) return ERR::NullArgs;

   return effect_edit(Self, [&](AudioParamState &State) {
      auto g = find_group(*Self->Schema, Args->Group);
      if (g < 0) return ERR::Search;
      auto &entries = State.Groups[g];
      if ((Args->Index < 0) or (size_t(Args->Index) >= entries.size())) return ERR::OutOfRange;
      if (int(entries.size()) <= Self->Schema->Groups[g].MinCount) return ERR::OutOfRange;
      entries.erase(entries.begin() + Args->Index);
      return ERR::Okay;
   });
}

/*********************************************************************************************************************

-METHOD-
SetParameter: Changes a parameter value by key.

Sets a parameter published in the #Schema.  The value is checked against the parameter's own range immediately.
Rules that relate parameters to each other are checked when the change is applied: by `Init()` before initialisation,
or by `Flush()` afterwards.  After initialisation the change is staged and has no effect on the processor until
`Flush()` is called.

Enumerated values are set with their numeric value.

-INPUT-
strview Path: A parameter key such as `gain` or `bands[2].frequency`.
double Value: The new value, in the parameter's unit.

-ERRORS-
Okay
NullArgs
NoSupport: The effect does not publish a schema.
Search: The key is malformed or names an unknown parameter.
OutOfRange: The group index does not refer to an existing entry.
InvalidValue: The value is outside the parameter's range.
Immutable: The effect is attached to the global chain.
NotInitialised: The effect has been disconnected from its Audio object.
-END-

*********************************************************************************************************************/

static ERR AUDIOEFFECT_SetParameter(extAudioEffect *Self, struct fx::SetParameter *Args)
{
   if (not Args) return ERR::NullArgs;
   const int rate = effect_rate(Self);

   return effect_edit(Self, [&](AudioParamState &State) {
      double *value;
      const AudioParamDesc *desc;
      if (auto error = resolve_path(*Self->Schema, State, Args->Path, value, desc); error != ERR::Okay) return error;
      if (auto error = check_param(*desc, Args->Value, rate); error != ERR::Okay) return error;
      *value = Args->Value;
      return ERR::Okay;
   });
}

/*********************************************************************************************************************

-FIELD-
Audio: Target Audio object, inherited from an Audio owner if omitted.

Set before initialisation.  Explicit targets take precedence over ownership.  Ownership changes after attachment do
not change the target.  Freeing the target disconnects all its effects, including effects owned by other objects.

-FIELD-
Channel: Channel-set handle, or zero for the global chain.

Set before initialisation.  Use the complete handle returned by @Audio.OpenChannels(), with a zero low word.
Individual channel handles and closed channel sets are rejected.

-FIELD-
Flags: Optional processing flags.
Lookup: AEF

*********************************************************************************************************************/

static ERR AUDIOEFFECT_SET_Flags(extAudioEffect *Self, AEF Value)
{
   if ((Value & ~AEF::BYPASS) != AEF::NIL) return ERR::InvalidValue;
   if (Self->initialised() and !Self->Channel) return ERR::Immutable;

   auto chain = Self->Chain.lock();
   if (!chain) {
      if (Self->initialised()) return ERR::NotInitialised;
      Self->Flags = Value;
      return ERR::Okay;
   }

   if (!Self->Channel and (Value != AEF::NIL)) return ERR::InvalidValue;
   std::lock_guard mixer_lock(*chain->Mutex);

   if (((Self->Flags & AEF::BYPASS) != AEF::NIL) and ((Value & AEF::BYPASS) IS AEF::NIL)) {
      Self->ResetPending = true;
   }

   Self->Flags = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Mutable: Read-only.  Zero if the effect's parameters can no longer be changed.

Returns zero for an initialised effect in the global chain or an initialised effect disconnected from its Audio
object or channel set.  Otherwise returns one.

*********************************************************************************************************************/

static ERR AUDIOEFFECT_GET_Mutable(extAudioEffect *Self, int *Value)
{
   *Value = (effect_mutable(Self) IS ERR::Okay) ? 1 : 0;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Order: Processing position within the chain.

Lower values run first.  Ties resolve by original attachment order, including after an application effect is reordered.
Global effects cannot change order after initialisation.

*********************************************************************************************************************/

static ERR AUDIOEFFECT_SET_Order(extAudioEffect *Self, int Value)
{
   if (Self->initialised() and !Self->Channel) return ERR::Immutable;
   auto chain = Self->Chain.lock();
   if (!chain) {
      if (Self->initialised()) return ERR::NotInitialised;
      Self->Order = Value;
      return ERR::Okay;
   }
   std::lock_guard mixer_lock(*chain->Mutex);
   Self->Order = Value;
   sort_effects(*chain);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OutputRate: Read-only output sample rate of the attached Audio object.

Updated when the Audio device is activated.  Processors reset before processing the new output configuration.

*********************************************************************************************************************/

static ERR AUDIOEFFECT_GET_OutputRate(extAudioEffect *Self, int *Value)
{
   if (auto chain = Self->Chain.lock()) {
      std::lock_guard mixer_lock(*chain->Mutex);
      *Value = Self->OutputRate;
   }
   else *Value = Self->OutputRate;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Stereo: Read-only output layout; one for stereo and zero for mono.

Updated on device activation together with #OutputRate.
-END-

*********************************************************************************************************************/

static ERR AUDIOEFFECT_GET_Stereo(extAudioEffect *Self, int *Value)
{
   if (auto chain = Self->Chain.lock()) {
      std::lock_guard mixer_lock(*chain->Mutex);
      *Value = Self->Stereo;
   }
   else *Value = Self->Stereo;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Schema: Read-only.  An XML description of the effect's parameters.

The schema describes every parameter that can be read or changed with #GetParameter() and #SetParameter(), including
units, ranges, defaults, repeated groups and the rules between parameters.  The format is described in the class
documentation.  The schema is the same for every instance of a class; instance-specific bounds, such as the Nyquist
frequency, are resolved against #OutputRate.

The value is empty if the class does not publish a schema.

*********************************************************************************************************************/

static ERR AUDIOEFFECT_GET_Schema(extAudioEffect *Self, std::string_view &Value)
{
   static std::mutex cache_lock;
   static std::unordered_map<const AudioEffectSchema *, std::string> cache;

   if (not Self->Schema) {
      Value = std::string_view();
      return ERR::Okay;
   }

   std::lock_guard lock(cache_lock);
   auto it = cache.find(Self->Schema);
   if (it IS cache.end()) it = cache.emplace(Self->Schema, build_schema_xml(*Self->Schema)).first;
   Value = it->second;
   return ERR::Okay;
}

//********************************************************************************************************************

#include "class_audioeffect_def.c"

static const FieldArray clAudioEffectFields[] = {
   { "Audio",      FDF_OBJECTID|FDF_RI, nullptr, nullptr, CLASSID::AUDIO },
   { "Channel",    FDF_INT|FDF_RI },
   { "Order",      FDF_INT|FDF_RW, nullptr, AUDIOEFFECT_SET_Order },
   { "Flags",      FDF_INT|FDF_FLAGS|FDF_RW, nullptr, AUDIOEFFECT_SET_Flags, &clAudioEffectFlags },
   { "OutputRate", FDF_INT|FDF_R, AUDIOEFFECT_GET_OutputRate },
   { "Stereo",     FDF_INT|FDF_R, AUDIOEFFECT_GET_Stereo },
   // Virtual fields
   { "Mutable",    FDF_VIRTUAL|FDF_INT|FDF_R, AUDIOEFFECT_GET_Mutable },
   { "Schema",     FDF_VIRTUAL|FDF_CPPSTRING|FDF_R, AUDIOEFFECT_GET_Schema },
   END_FIELD
};

static ERR add_audioeffect_class()
{
   clAudioEffect = objMetaClass::create::global(
      fl::ClassVersion(VER_AUDIOEFFECT),
      fl::Name("AudioEffect"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioEffectActions),
      fl::Methods(clAudioEffectMethods),
      fl::Fields(clAudioEffectFields),
      fl::Size(sizeof(extAudioEffect)),
      fl::Path(MOD_PATH));
   return clAudioEffect ? ERR::Okay : ERR::AddClass;
}
