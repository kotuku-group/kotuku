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

<header>Parallel Branches</header>

Each chain processes its effects one after another.  To divide the signal and process it along separate paths, attach
a container effect such as @AudioSplitter to the chain.  A container hosts a set of branches, each being an ordinary
chain of effects, and sums their outputs.  The container is a single node in its chain, so effects can run before
and after it.

An effect is attached to a branch by setting its #Parent to the container and its #Branch to the branch index before
initialisation, or by creating it as a child of the container.  It inherits the container's #Audio and #Channel, and
is ordered by #Order within its branch.  Branch effects publish meters, accept live changes and can be bypassed in the
same way as effects in a top-level chain.  Containers cannot be nested.

<header>Stop Notifications</header>

Effects such as reverbs and delays continue to produce output after their source stops.  When a voice's path has
effects, being its channel set's chain followed by the global chain, the sample's `OnStop` notification and the
@Sound.OnStop callback are held until that output has drained.  They are then delivered after the device queue, as
for a path without effects, so the callback marks the approximate end of everything that the voice made audible.

<header>Parameter Schema</header>

Subclasses can publish their parameters in the #Schema field, so that a client can present and change any effect
without prior knowledge of class attributes.  The schema is an XML document:

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
#GetResponse().  The `spectrum` and `waveform` outputs of an @AudioAnalyser are read with its own methods.  Scalar
outputs are meter values, read together with #ReadMeters() or individually with #GetOutput().
Their `label`, `description`, `unit`, `scope`, `semantics` and zero-based `slot` attributes describe the value and its
location in the current meter layout.  A `channel` scope identifies a value measured on one channel of the processing
layout, and its `channel` attribute holds the channel identity from the `SPK` constants.  A `global` scope identifies a
value that applies to every channel, such as linked gain reduction.  Keys are stable, e.g. `input_peak_left`, and only
channels present in the processing layout are listed.  Sample peaks are not true-peak measurements.  Gain reduction,
where offered, is non-negative attenuation in decibels.

The `slot` attributes describe the meter layout that was current when the schema was read.  A change of processing
layout replaces the meter layout; use #GetMeterLayout() to resolve keys to slots, and repeat the resolution whenever
#ReadMeters() reports a different `ID`.

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

static ERR mr_free(ResourceRecord &Resource, APTR Address)
{
   ((struct MeterReading *)Address)->~MeterReading();
   return ERR::Terminate;
}

static ERR ml_free(ResourceRecord &Resource, APTR Address)
{
   ((struct MeterLayout *)Address)->~MeterLayout();
   return ERR::Terminate;
}

static ResourceManager glMeterLayoutHandler = { "MeterLayout", &ml_free, false };
static ResourceManager glMeterReadingHandler = { "MeterReading", &mr_free, false };

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
      int rate, stereo;
      uint64_t generation = 0;
      {
         std::unique_lock<std::recursive_mutex> mixer_lock;
         if (chain) mixer_lock = std::unique_lock(*chain->Mutex);
         rate = Self->OutputRate;
         stereo = Self->Stereo;
         if (chain) generation = *chain->Generation;
      }
      if (validate_state(*Self->Schema, State, rate, &committed) != ERR::Okay) {
         return ERR::InvalidValue;
      }
      auto update = Self->Schema->Prepare ? Self->Schema->Prepare(Self, State, rate) : nullptr;
      {
         std::unique_lock<std::recursive_mutex> mixer_lock;
         if (chain) mixer_lock = std::unique_lock(*chain->Mutex);
         if (Self->OutputRate != rate or Self->Stereo != stereo or
             (chain and *chain->Generation != generation)) continue;
         if (chain and chain->Rate > 0 and update and update->latency() >= 0 and
             update->latency() != Self->CommittedLatency) {
            return ERR::InvalidState;
         }
         if (update) update->publish(Self);
         else Self->Schema->Apply(Self, State);
         if (update and update->latency() >= 0 and update->latency() != Self->CommittedLatency) {
            Self->CommittedLatency = update->latency();
         }
         if (chain) Self->reset_meter(++*chain->Generation);
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
InvalidState: A staged edit would change the configured processor instance's algorithmic latency.
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
   if (not Args) return ERR::NullArgs;
   if ((not Self->initialised()) and Args->NewOwner) {
      if ((not Self->AudioID) and (Args->NewOwner->Class->BaseClassID IS CLASSID::AUDIO)) {
         Self->AudioID = Args->NewOwner->UID;
      }
      else if ((not Self->ParentID) and is_effect_container(Args->NewOwner->Class->ClassID)) {
         Self->ParentID = Args->NewOwner->UID;
      }
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Resolves the container and branch for an effect with a Parent.  Audio and Channel are inherited from the container.
// Called with the container's object lock held; the branch is resolved again under the mixer lock.

static ERR effect_parent(extAudioEffect *Self, extAudioEffect *Parent)
{
   kt::Log log(__FUNCTION__);

   if (is_effect_container(Self->Class->ClassID)) {
      log.warning("A container effect cannot be attached to the branch of another container.");
      return ERR::NoSupport;
   }
   if ((not is_effect_container(Parent->Class->ClassID)) or (not Parent->initialised())) return ERR::InvalidObject;
   if (Self->AudioID and (Self->AudioID != Parent->AudioID)) return ERR::InvalidValue;
   if (Self->Channel and (Self->Channel != Parent->Channel)) return ERR::InvalidValue;
   Self->AudioID = Parent->AudioID;
   Self->Channel = Parent->Channel;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR AUDIOEFFECT_Init(extAudioEffect *Self)
{
   // A failed derived Init may be retried; never register the same object twice.

   Self->detach();

   if ((not Self->ParentID) and Self->Owner and is_effect_container(Self->Owner->Class->ClassID)) {
      Self->ParentID = Self->Owner->UID;
   }

   // The container stays locked until attachment is complete, so that it cannot be freed in the meantime.

   std::optional<kt::ScopedObjectLock<extAudioEffect>> parent;
   if (Self->ParentID) {
      parent.emplace(Self->ParentID, 3000);
      if (not parent->granted()) return ERR::Search;
      if ((*parent)->Class->BaseClassID != CLASSID::AUDIOEFFECT) return ERR::InvalidObject;
      if (auto error = effect_parent(Self, **parent); error != ERR::Okay) return error;
   }
   else if ((not Self->AudioID) and Self->Owner and (Self->Owner->Class->BaseClassID IS CLASSID::AUDIO)) {
      Self->AudioID = Self->Owner->UID;
   }

   if (not Self->AudioID) return ERR::FieldNotSet;
   if ((Self->Flags & ~AEF::BYPASS) != AEF::NIL) return ERR::InvalidValue;
   if ((not Self->Channel) and (Self->Flags != AEF::NIL)) return ERR::InvalidValue;

   kt::ScopedObjectLock<extAudio> audio(Self->AudioID, 3000);

   if (not audio.granted()) return ERR::Search;
   if (audio->Class->BaseClassID != CLASSID::AUDIO) return ERR::InvalidObject;

   std::lock_guard mixer_lock(audio->MixerMutex);
   auto chain = audio->GlobalEffects;
   if (parent) {
      // A container disconnected from its Audio object or channel set no longer hosts branches.
      auto container = **parent;
      if (container->Chain.expired() or container->Branches.empty()) return ERR::NotInitialised;
      if ((Self->Branch < 0) or (Self->Branch >= std::ssize(container->Branches))) return ERR::OutOfRange;
      chain = container->Branches[Self->Branch];
   }
   else if (Self->Channel) {
      const int index = Self->Channel >> 16;
      if ((Self->Channel < 0) or (Self->Channel & 0xffff) or (index < 1) or
          (index >= std::ssize(audio->Sets)) or audio->Sets[index].Channel.empty()) return ERR::Args;
      auto &set = audio->Sets[index];
      if (not set.Effects) set.Effects = std::make_shared<AudioEffectChain>(audio->MixerLock);
      set.ScratchBuffer.resize(audio->MixBuffer.size());
      chain = set.Effects;
      chain->Generation = audio->GlobalEffects->Generation;
   }

   // A committed output configuration is adopted directly.  Otherwise the requested layout is provisional until the
   // Audio object is activated, when every chain is configured with the committed layout.

   chain->Rate = audio->EffectConfigured ? audio->OutputRate : 0;
   if (audio->EffectConfigured) {
      chain->Layout = audio->CommittedLayout;
      chain->FormatGeneration = audio->OutputGeneration;
   }
   else chain->Layout = audio->OutputLayout;
   chain->Stereo = chain->Layout.size() IS 2;

   Self->OutputRate       = audio->OutputRate;
   Self->FormatCommitted  = audio->EffectConfigured;
   Self->FormatGeneration = audio->EffectConfigured ? audio->OutputGeneration : 0;
   Self->set_layout(chain->Layout);
   Self->reset_meter(++*chain->Generation);
   Self->ResetPending = true;
   Self->Sequence     = chain->NextSequence++;
   Self->Chain        = chain;

   chain->Effects.push_back(Self);
   sort_effects(*chain);
   ++*chain->Generation;
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
GetProcessingFormat: Reads the PCM format processed by the effect.

GetProcessingFormat() reports the committed format of the audio that passes through the effect.  Effects process the
Audio object's floating-point mix, so `SampleFormat` is always `ASF::F32` in native byte order.  The input and output
layouts of an effect are always identical.  Channel identities are listed in interleaved frame order.

The format is committed when the @Audio object is activated.  An effect attached to an inactive Audio object has no
committed format until activation, in which case `State` is `AFS::UNAVAILABLE` and the remaining results are empty.
After the Audio object is deactivated, or if the effect is disconnected, the last committed format remains available and
`State` is `AFS::INACTIVE`.

!AFS

`Generation` identifies the committed output configuration and matches the value reported by
@Audio.GetOutputFormat() for the same configuration.  It changes whenever the Audio object commits a new processing
configuration, including reactivation with an unchanged layout.  Meter layouts follow the processing layout; see
#GetMeterLayout().

-INPUT-
&int SampleRate: Frames per second, or zero if no format is available.
&int(ASF) SampleFormat: Sample representation of the processed audio.
^&vector(int) Layout: Receives the ordered channel identities from the `SPK` constants.
&large Generation: Output configuration generation of the reported format.
&int(AFS) State: Availability of the reported format.

-ERRORS-
Okay
NullArgs

-END-

*********************************************************************************************************************/

static ERR AUDIOEFFECT_GetProcessingFormat(extAudioEffect *Self, struct fx::GetProcessingFormat *Args)
{
   if (not Args) return ERR::NullArgs;

   Args->SampleRate = 0;
   Args->SampleFormat = ASF::NIL;
   Args->Generation = 0;
   Args->State = AFS::UNAVAILABLE;
   if (Args->Layout) Args->Layout->clear();

   auto chain = Self->Chain.lock();
   std::unique_lock<std::recursive_mutex> lock;
   if (chain) lock = std::unique_lock(*chain->Mutex);

   if (not Self->FormatCommitted) return ERR::Okay;

   Args->SampleRate   = Self->OutputRate;
   Args->SampleFormat = ASF::F32;
   Args->Generation   = Self->FormatGeneration;
   Args->State        = (chain and chain->Rate > 0) ? AFS::ACTIVE : AFS::INACTIVE;
   if (Args->Layout) Args->Layout->assign(Self->Layout.begin(), Self->Layout.end());
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetMeterLayout: Describes the values published by the effect's meters.

Call GetMeterLayout() to discover the meter values that the effect measures, typically to build level meters in a user
interface.  The result is a !MeterLayout snapshot containing a configuration identifier and a `Meters` array of
!MeterInfo records in slot order.  The `Slot` of each record is the index of its value in the `Values` of a
reading from #ReadMeters().

Each record gives the value's stable `Key`, a human-readable `Label`, its `Unit` (e.g. `dBFS`), a `Scope` of either
`channel` or `global`, and the `Semantics` of the measurement (e.g. `sample-peak`).  Values measured on one channel
carry that channel's identity from the `SPK` constants in `Channel`, while global values such as linked gain reduction
have a `Channel` of zero.  Missing units or semantics are empty strings.

Descriptors are published only for the channels in the effect's processing layout, e.g. `input_peak_left` and
`input_peak_right` for stereo, or `input_peak_centre` for mono.  Slots are dense and stable within one layout
configuration.  Stable keys and channel identities, not slot numbers, preserve a value's meaning when the layout changes.

`ID` is the configuration identifier that the descriptors belong to, and uses the same definition as the
`ID` of a !MeterReading result.  Keep it with the descriptors and compare it with the `ID` of each
reading from #ReadMeters().  If the two differ, call GetMeterLayout() again before interpreting the values.  Any change
to the meter layout, e.g. activating the @Audio object with a different output layout, changes the ID.  Other
configuration changes, such as committing a parameter or toggling bypass, change it too, so a client can re-read an
unchanged layout; this is harmless.

The descriptors and their ID are copied together.  The snapshot owns its records and strings, so it remains
valid after the effect's layout changes or the effect is freed.  The snapshot is allocated by the effect and must be
released with ~Core.FreeResource() by C++ clients.

-INPUT-
!struct(*MeterLayout) Layout: Receives an owned snapshot of the meter descriptors and their ID.

-ERRORS-
Okay
NullArgs
AllocMemory

-END-

*********************************************************************************************************************/

static ERR AUDIOEFFECT_GetMeterLayout(extAudioEffect *Self, struct fx::GetMeterLayout *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   Args->Layout = nullptr;

   // Allocate before taking the mixer lock, as for ReadMeters().  Copy the descriptors while locked so no pointer
   // into the effect's storage escapes to the caller or the script marshaller.

   struct MeterLayout *layout;
   if (AllocResource(sizeof(struct MeterLayout), MEM::NIL, (APTR *)&layout, &glMeterLayoutHandler) != ERR::Okay) {
      return ERR::AllocMemory;
   }
   new (layout) struct MeterLayout;

   auto chain = Self->Chain.lock();
   std::unique_lock<std::recursive_mutex> lock;
   if (chain) lock = std::unique_lock(*chain->Mutex);

   layout->Meters.reserve(Self->Meters.size());
   for (const auto &meter : Self->Meters) layout->Meters.push_back(meter);
   layout->ID = int64_t(Self->meter_generation(chain.get()));
   Args->Layout = layout;

   return ERR::Okay;
}

/*********************************************************************************************************************
-METHOD-
GetOutput: Reads one value from a schema key.

Returns the latest completed interval value for a `Key`.  Keys are resolved against the current meter layout, so a
key for a channel that is absent from the processing layout is not found.  GetOutput() is a convenience for reading a
single value; use #ReadMeters() for coherent multi-value reads and lifecycle flags.  Curve outputs continue to use
#GetResponse().

-INPUT-
strview Key: Scalar output key from #Schema or #GetMeterLayout().
&double Value: Latest value in the descriptor's units.

-ERRORS-
Okay
NullArgs
Search: Unknown key, including keys for channels absent from the processing layout.
NoSupport: The key names a curve or another unsupported output kind.
NotInitialised: The effect is disconnected.
InvalidState: No current valid measurement is available, or the effect is idle or bypassed.

-END-
*********************************************************************************************************************/

static ERR AUDIOEFFECT_GetOutput(extAudioEffect *Self, struct fx::GetOutput *Args)
{
   if (not Args) return ERR::NullArgs;

   auto chain = Self->Chain.lock();
   if (not chain) return ERR::NotInitialised;
   std::lock_guard lock(*chain->Mutex);

   for (size_t slot = 0; slot < Self->Meters.size(); slot++) {
      if (Args->Key != Self->Meters[slot].Key) continue;
      if ((Self->Flags & AEF::BYPASS) != AEF::NIL or Self->Meter.Flags != AMF::VALID) return ERR::InvalidState;
      Args->Value = Self->Meter.Values[slot];
      return ERR::Okay;
   }

   if (Self->Schema) for (const auto &output : Self->Schema->Outputs) {
      if ((Args->Key IS output.Key) and (output.Kind != AudioOutputKind::SCALAR)) return ERR::NoSupport;
   }

   return ERR::Search;
}

/*********************************************************************************************************************
-METHOD-
ReadMeters: Reads one coherent measurement of every meter value.

ReadMeters() returns the effect's latest measurement interval as a !MeterReading structure.  All values and metadata
describe the same interval, so readings from different intervals are never mixed.  Use #GetOutput() instead when only
one value is needed.

`Values` holds one element per meter descriptor, indexed by the `Slot` of the descriptors from #GetMeterLayout().
Compare the reading's `ID` with the ID returned by #GetMeterLayout().  If they differ, the layout may
have changed since the descriptors were read, e.g. after a device reconfiguration, so call #GetMeterLayout() again
before interpreting the values.

Peaks are sample peaks in dBFS, floored at -120 dBFS.  Gain reduction is non-negative attenuation in dB.  The `Flags`
of each element of `Values` is a combination of the following:

!AMV

Intervals contain `ceil(OutputRate / 20)` frames (50 ms rounded up).  Reads are non-destructive; slow readers can
miss intervals.  Idle publishes a partial final interval.  Reset and reconfiguration invalidate measurements.
`Sequence` increases on publication.  `Position` counts processed output frames since reset; `Interval` is the number
of frames represented.  `ID` identifies configuration and path changes, including changes to the meter layout.

The reading's `Flags` reports the snapshot's validity and lifecycle state.  Bypassed and disconnected snapshots do not
set `VALID`, and none of their values are flagged as valid.

!AMF

The reading is allocated by the effect and must be released with ~Core.FreeResource() by C++ clients.

-INPUT-
!struct(*MeterReading) Reading: Receives the latest measurement.

-ERRORS-
Okay
NullArgs
AllocMemory
NotInitialised: The effect has not been initialised.

-END-
*********************************************************************************************************************/

static ERR AUDIOEFFECT_ReadMeters(extAudioEffect *Self, struct fx::ReadMeters *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   Args->Reading = nullptr;

   // Allocate before taking the mixer lock, which the render thread also needs.

   struct MeterReading *reading;
   if (AllocResource(sizeof(struct MeterReading), MEM::NIL, (APTR *)&reading, &glMeterReadingHandler) != ERR::Okay) {
      return ERR::AllocMemory;
   }
   new (reading) struct MeterReading;

   auto chain = Self->Chain.lock();
   std::unique_lock<std::recursive_mutex> lock;
   if (chain) lock = std::unique_lock(*chain->Mutex);

   Self->read_meter(chain.get(), *reading);
   Args->Reading = reading;
   return ERR::Okay;
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
Branch: Zero-based index of the container branch that hosts the effect.

Set Branch before initialisation, together with #Parent, to select the branch of a container effect such as
@AudioSplitter that the effect is attached to.  Initialisation fails with `ERR::OutOfRange` if the branch does not
exist.  The field is ignored if #Parent is zero.

Branch indexes change when the container's `branches` group is edited: removing an earlier branch moves the effect
down by one, and inserting an earlier branch moves it up by one.  The effect stays attached to the same branch and
reading this field returns its current index.  After the effect's branch is removed, the field retains the last index
that the effect had.

*********************************************************************************************************************/

static ERR AUDIOEFFECT_GET_Branch(extAudioEffect *Self, int *Value)
{
   // Containers renumber their branch effects under the mixer lock.
   if (auto chain = Self->Chain.lock()) {
      std::lock_guard mixer_lock(*chain->Mutex);
      *Value = Self->Branch;
   }
   else *Value = Self->Branch;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Channel: Channel-set handle, or zero for the global chain.

Set before initialisation.  Use the complete handle returned by @Audio.OpenChannels(), with a zero low word.
Individual channel handles and closed channel sets are rejected.  This identifies a set of voice channels, not a
speaker; the effect always processes every speaker channel of its processing layout.

-FIELD-
Flags: Optional processing flags.
Lookup: AEF

*********************************************************************************************************************/

static ERR AUDIOEFFECT_SET_Flags(extAudioEffect *Self, AEF Value)
{
   if ((Value & ~AEF::BYPASS) != AEF::NIL) return ERR::InvalidValue;

   auto chain = Self->Chain.lock();
   if (not chain) {
      Self->Flags = Value;
      return ERR::Okay;
   }

   if (!Self->Channel and (Value != AEF::NIL)) return ERR::InvalidValue;
   std::lock_guard mixer_lock(*chain->Mutex);

   if (((Self->Flags & AEF::BYPASS) != AEF::NIL) and ((Value & AEF::BYPASS) IS AEF::NIL)) {
      Self->ResetPending = true;
   }

   if (Self->Flags != Value) Self->reset_meter(++*chain->Generation);
   Self->Flags = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Latency: Returns committed algorithmic delay in output frames.

Latency reports the fixed delay, measured in frames at the #OutputRate, that the effect's processor adds to the signal
path.  The value should not be read until after the processor is configured.  Effects with no look-ahead or buffering,
such as the equaliser, report zero.

Setting the `BYPASS` flag reduces the reported latency to zero, because a bypassed effect does not delay the signal.
The delay of a complete chain, including the global chain, can be read with the Audio class' `GetEffectStatus()`
method.

*********************************************************************************************************************/

static ERR AUDIOEFFECT_GET_Latency(extAudioEffect *Self, int64_t *Value)
{
   auto chain = Self->Chain.lock();
   if (not chain) return ERR::NotInitialised;
   std::lock_guard lock(*chain->Mutex);
   if (not chain->Rate) return ERR::NotInitialised;
   *Value = Self->latency();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Mutable: Indicates whether the effect's parameters can be changed.

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
   if (not chain) {
      if (Self->initialised()) return ERR::NotInitialised;
      Self->Order = Value;
      return ERR::Okay;
   }

   std::lock_guard mixer_lock(*chain->Mutex);
   Self->Order = Value;
   sort_effects(*chain);
   ++*chain->Generation;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OutputRate: Output sample rate of the attached Audio object.

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
Parent: The container effect that hosts this effect, or zero for an effect in a top-level chain.

Set Parent before initialisation to attach the effect to a branch of a container effect such as @AudioSplitter,
instead of attaching it directly to an Audio chain.  The #Branch field selects the branch.  Creating the effect as a
child of a container sets Parent automatically.

The container must be initialised.  The effect inherits the container's #Audio and #Channel; if either field has been
set to a different value, initialisation fails with `ERR::InvalidValue`.  Like the container, the effect is immutable
after initialisation if it belongs to the global chain.  A container cannot be attached to the branch of another
container.

Within its branch, the effect is ordered by #Order.  Removing the branch from the container, freeing the container, or
disconnecting the container from its Audio object or channel set disconnects the effect.

-FIELD-
Schema: An XML description of the effect's parameters.

The schema describes every parameter that can be read or changed with #GetParameter() and #SetParameter(), including
units, ranges, defaults, repeated groups and the rules between parameters.  The format is described in the class
documentation.  Scalar outputs are listed for the channels of the current processing layout, with slots from the
current meter layout.  Instance-specific bounds, such as the Nyquist frequency, are resolved against #OutputRate.

The value is empty if the class does not publish a schema.

-END-
*********************************************************************************************************************/

static ERR AUDIOEFFECT_GET_Schema(extAudioEffect *Self, std::string_view &Value)
{
   // A document depends only on the schema and the processing layout, so it is cached per distinct pair.  Cached
   // strings are never released, so a returned view remains valid.

   static std::mutex cache_lock;
   static std::map<std::pair<const AudioEffectSchema *, std::vector<int>>, std::string> cache;

   if (not Self->Schema) {
      Value = std::string_view();
      return ERR::Okay;
   }

   std::vector<AudioMeterDesc> meters;
   std::vector<int> layout;
   {
      auto chain = Self->Chain.lock();
      std::unique_lock<std::recursive_mutex> mixer_lock;
      if (chain) mixer_lock = std::unique_lock(*chain->Mutex);
      meters = Self->Meters;
      layout = Self->Layout;
   }

   std::lock_guard lock(cache_lock);
   auto &xml = cache[{ Self->Schema, std::move(layout) }];
   if (xml.empty()) xml = build_schema_xml(*Self->Schema, meters);
   Value = xml;
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
   { "Parent",     FDF_OBJECTID|FDF_RI, nullptr, nullptr, CLASSID::AUDIOEFFECT },
   { "Branch",     FDF_INT|FDF_RI, AUDIOEFFECT_GET_Branch },
   // Virtual fields
   { "Latency",    FDF_VIRTUAL|FDF_INT64|FDF_R, AUDIOEFFECT_GET_Latency },
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
