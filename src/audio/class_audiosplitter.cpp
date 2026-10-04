/*********************************************************************************************************************

-CLASS-
AudioSplitter: Processes copies of a signal along parallel branches of effects and sums the results.

AudioSplitter divides an effect chain into parallel paths.  It sends an identical copy of its input through each of
its branches, where every branch is an ordinary chain of effects, and adds the branch outputs together.  The splitter
is a single node in its own chain, so effects can run before and after it.  Typical uses include:

<list type="bullet">
<li>Parallel compression: an empty branch carries the dry signal, while a second branch holds an @AudioCompressor.
</li>
<li>Bass enhancement: one branch processes the full signal, while a second branch isolates the bass with the
`low_pass` and `high_pass` bands of an @AudioEqualiser and shapes it with further effects.</li>
<li>Wet and dry mixing of any effect, with independent levels for each path.</li>
</list>

Create the splitter as a child of an @Audio object or set the inherited @AudioEffect.Audio field.  Use
@AudioEffect.Channel to process one channel set, or leave it at zero to process the global mix.  Application splitters
accept live changes; global splitters and the effects in their branches become immutable after initialisation.

<header>Branches</header>

Branches are published through the inherited @AudioEffect schema as the `branches` group, which holds between one
and eight entries.  Each entry has two parameters:

<list type="bullet">
<li>`gain`: the level of the branch output in the sum, from -48 to +12 dB.</li>
<li>`mute`: when set, the branch is removed from the sum.  A muted branch continues to process its input, so its
effects keep their state and meters.</li>
</list>

A new splitter has one unmuted branch, so it passes audio through unchanged.  Branches inserted with
@AudioEffect.InsertEntry() are muted until they are enabled, so that inserting a branch is inaudible.  An empty
branch passes its copy of the input through unchanged.

An effect is attached to a branch by setting its @AudioEffect.Parent field to the splitter and its @AudioEffect.Branch
field to the branch index, or by creating it as a child of the splitter.  The splitter must be initialised first.
Within a branch, effects are ordered by @AudioEffect.Order.  The following builds a parallel compressor on a channel
set:

<pre>
splitter = obj.new('AudioSplitter', { audio=audio, channel=channel })
splitter.mtInsertEntry('branches', 1)
splitter.acSetKey('branches[1].mute', 0)
splitter.acSetKey('branches[1].gain', -6)
splitter.acFlush()
compressor = splitter.new('AudioCompressor', { branch=1 })
</pre>

Removing a branch disconnects the effects in that branch, and later branches move down by one.  Effects follow their
branch when other branches are inserted or removed, and @AudioEffect.Branch reports the current index.  Freeing the
splitter, or disconnecting it from its Audio object or channel set, disconnects every branch effect.  A splitter cannot
be placed inside the branch of another splitter.

<header>Levels</header>

The sum is not normalised: two unprocessed branches at 0 dB produce a signal 6 dB louder than the input.  Lower the
branch gains, or place an @AudioLimiter after the splitter, when the branches can add up to more than full scale.

<header>Latency Alignment</header>

Effects such as @AudioLimiter delay the signal.  If the branches had different delays, their sum would suffer from
comb filtering, so the splitter delays each branch to align it with the others.  Every branch emerges after a fixed
#LatencyBudget, which the splitter reports as its @AudioEffect.Latency.  The budget is zero by default, which permits
only effects without latency in the branches.

Set the budget before initialisation to at least the greatest combined latency of the effects in any one branch.
Attaching an effect that would take its branch beyond the budget fails with `ERR::OutOfRange`.  If an effect is
attached before the Audio object has been activated, the check is made at activation, which reports any excess in its
log; the excess delays that branch beyond the others.

Latency is counted whether or not an effect is bypassed.  When bypass, attachment or detachment changes the latency
of a branch, the branch moves to its new alignment with a 10 ms crossfade.

<header>Live Changes</header>

Gain and mute changes ramp over 10 ms.  An inserted branch starts at zero gain and ramps in.  A removed branch ramps
out over 10 ms, during which its former effects continue to process the signal although they are already
disconnected.  Bypassing the splitter skips every branch; leaving bypass, device reactivation and reaching idle
discard alignment history and reset every branch effect.

<header>Drain</header>

Sample @Sound.OnStop notifications are held until the tails of the effects in audible branches, such as a reverb, have
drained through the splitter.  Muted branches do not hold notifications.

<header>Meters</header>

The splitter publishes the standard input and output peaks for each channel, measured before and after the sum.  The
levels of individual branches are read from the meters of the effects inside them.

-END-

*********************************************************************************************************************/

#include "audio_splitter_dsp.h"

class extAudioSplitter : public extAudioEffect {
public:
   std::vector<SplitterBranchSettings> Settings = { { 0, false } };
   SplitterProcessor *Processor = nullptr;
   double LatencyBudget = 0;

   extAudioSplitter(objMetaClass *ClassPtr, OBJECTID ObjectID);
};

//********************************************************************************************************************
// Parameter descriptors.  Member order matches the BRANCH_ indexes.

enum { BRANCH_GAIN = 0, BRANCH_MUTE };
enum { SPLITTER_BRANCHES = 0 };

static const AudioParamDesc glBranchMembers[] = {
   { .Key = "gain", .Label = "Gain", .Description = "The level of the branch output in the sum.",
     .Unit = APU::DB, .Min = -48, .Max = 12, .Default = 0 },
   { .Key = "mute", .Label = "Mute",
     .Description = "Removes the branch from the sum.  A muted branch continues to process its input, so that its "
        "effects keep their state.",
     .Type = APT::BOOL, .Min = 0, .Max = 1, .Default = 1 }
};

static const AudioParamGroup glSplitterGroups[] = {
   { .Key = "branches", .Label = "Branch",
     .Description = "Parallel paths that each process a copy of the input.  The branch outputs are added together.  "
        "A new branch is muted until it is enabled.",
     .MinCount = 1, .MaxCount = SPLITTER_MAX_BRANCHES, .Members = glBranchMembers }
};

static const AudioOutputDesc glSplitterOutputs[] = {
   { "input_peak", AudioOutputKind::SCALAR, "Input Peak", "Maximum input sample peak.", "dBFS", "channel",
      "sample-peak", AudioMeterSource::INPUT_PEAK },
   { "output_peak", AudioOutputKind::SCALAR, "Output Peak", "Maximum sample peak of the summed branches.", "dBFS",
      "channel", "sample-peak", AudioMeterSource::OUTPUT_PEAK }
};

//********************************************************************************************************************

static SplitterBranchSettings entry_branch(const AudioParamEntry &Entry)
{
   return SplitterBranchSettings { .Gain = Entry.Values[BRANCH_GAIN], .Mute = Entry.Values[BRANCH_MUTE] != 0 };
}

static void splitter_read(extAudioEffect *Effect, AudioParamState &State)
{
   auto Self = (extAudioSplitter *)Effect;
   State.Params.clear();
   State.Groups.resize(1);
   auto &entries = State.Groups[SPLITTER_BRANCHES];
   entries.clear();
   entries.reserve(Self->Settings.size());
   for (const auto &branch : Self->Settings) entries.push_back({ { branch.Gain, branch.Mute ? 1.0 : 0.0 } });
}

//********************************************************************************************************************
// Before initialisation there are no branch chains.  Init() creates one for each entry.

static void splitter_apply(extAudioEffect *Effect, const AudioParamState &State)
{
   auto Self = (extAudioSplitter *)Effect;
   Self->Settings.clear();
   for (const auto &entry : State.Groups[SPLITTER_BRANCHES]) Self->Settings.push_back(entry_branch(entry));
}

//********************************************************************************************************************
// Creates an unconfigured branch chain.  Configuration is copied from the container's chain under the mixer lock.

static std::shared_ptr<AudioEffectChain> new_branch(extAudioSplitter *Self,
   const std::shared_ptr<std::recursive_mutex> &Mutex, size_t LayoutSize)
{
   auto branch = std::make_shared<AudioEffectChain>(Mutex);
   branch->Container = Self->UID;
   branch->LatencyBudget = Self->LatencyBudget;
   branch->Layout.reserve(LayoutSize);
   return branch;
}

// Caller holds the mixer lock.

static void configure_branch(AudioEffectChain &Branch, const AudioEffectChain &Parent)
{
   Branch.Generation = Parent.Generation;
   Branch.Rate = Parent.Rate;
   Branch.Stereo = Parent.Stereo;
   Branch.Layout.assign(Parent.Layout.begin(), Parent.Layout.end());
   Branch.FormatGeneration = Parent.FormatGeneration;
}

//********************************************************************************************************************
// New branch chains and the storage that captures the processors of removed branches are allocated outside the mixer
// lock.  The chain generation is rechecked before publication, so effects attached or detached in the meantime cause
// the update to be prepared again.  Branches that publication replaces are retired here and released, disconnecting
// their effects, after unlocking.

class SplitterUpdate final : public AudioParamUpdate {
public:
   std::vector<SplitterBranchSettings> Settings;
   std::vector<std::shared_ptr<AudioEffectChain>> Branches;
   SplitterEdit Edit;

   SplitterUpdate(extAudioSplitter *Self, const AudioParamState &State) {
      const auto &entries = State.Groups[SPLITTER_BRANCHES];
      for (const auto &entry : entries) {
         Settings.push_back(entry_branch(entry));
         Edit.Origins.push_back(entry.Origin);
         Edit.Gains.push_back(splitter_gain(Settings.back()));
      }

      auto chain = Self->Chain.lock();
      auto mutex = chain ? chain->Mutex : std::make_shared<std::recursive_mutex>();
      size_t previous = 0, layout = 2;
      {
         std::unique_lock<std::recursive_mutex> mixer_lock(*mutex);
         previous = Self->Branches.size();
         Edit.Captures.resize(previous);
         for (size_t i = 0; i < previous; i++) Edit.Captures[i].reserve(Self->Branches[i]->Effects.size());
         if (chain) layout = std::max(layout, chain->Layout.size());
      }

      Branches.resize(entries.size());
      for (size_t i = 0; i < entries.size(); i++) {
         if ((entries[i].Origin < 0) or (size_t(entries[i].Origin) >= previous)) {
            Branches[i] = new_branch(Self, mutex, layout);
         }
      }
      Edit.Trash.reserve(SPLITTER_SLOTS);
   }

   void publish(extAudioEffect *Effect) override {
      auto Self = (extAudioSplitter *)Effect;
      auto parent = Self->Chain.lock();
      for (size_t i = 0; i < Branches.size(); i++) {
         const int origin = Edit.Origins[i];
         if ((origin >= 0) and (size_t(origin) < Self->Branches.size())) Branches[i] = Self->Branches[origin];
         else if (parent) configure_branch(*Branches[i], *parent);

         Branches[i]->Branch = int(i);
         for (auto effect : Branches[i]->Effects) effect->Branch = int(i);
      }

      if (Self->Processor) Self->Processor->update(Edit, Branches);
      Self->Branches.swap(Branches);
      Self->Settings.swap(Settings);
   }
};

static std::unique_ptr<AudioParamUpdate> splitter_prepare(extAudioEffect *Effect, const AudioParamState &State,
   int Rate)
{
   return std::make_unique<SplitterUpdate>((extAudioSplitter *)Effect, State);
}

static size_t splitter_group_count(extAudioEffect *Effect, size_t Group)
{
   return ((extAudioSplitter *)Effect)->Settings.size();
}

//********************************************************************************************************************

static const AudioEffectSchema glSplitterSchema = {
   .ClassName   = "AudioSplitter",
   .Version     = 1,
   .Description = "Processes copies of the signal along parallel branches of effects and sums the results.",
   .Groups      = glSplitterGroups,
   .Outputs     = glSplitterOutputs,
   .Read        = splitter_read,
   .Apply       = splitter_apply,
   .Prepare     = splitter_prepare,
   .GroupCount  = splitter_group_count
};

//********************************************************************************************************************

extAudioSplitter::extAudioSplitter(objMetaClass *ClassPtr, OBJECTID ObjectID) : extAudioEffect(ClassPtr, ObjectID)
{
   Schema = &glSplitterSchema;
}

//********************************************************************************************************************
// Branch chains are created before the processor so that effects can attach as soon as Init() returns.

static ERR AUDIOSPLITTER_Init(extAudioSplitter *Self)
{
   if (Self->Processor) return ERR::InvalidState;
   auto chain = Self->Chain.lock();
   if (not chain) return ERR::NotInitialised;

   std::vector<std::shared_ptr<AudioEffectChain>> branches;
   std::vector<double> gains;
   for (const auto &settings : Self->Settings) {
      branches.push_back(new_branch(Self, chain->Mutex, 2));
      gains.push_back(splitter_gain(settings));
   }

   AudioParamState state;
   splitter_read(Self, state);
   {
      std::lock_guard mixer_lock(*chain->Mutex);
      if (validate_state(glSplitterSchema, state, Self->OutputRate) != ERR::Okay) return ERR::InvalidValue;
      for (size_t i = 0; i < branches.size(); i++) {
         configure_branch(*branches[i], *chain);
         branches[i]->Branch = int(i);
      }
      Self->Branches = branches;
   }

   auto processor = std::make_unique<SplitterProcessor>(Self, Self->LatencyBudget, branches, gains);
   auto pointer = processor.get();

   auto error = Self->set_processor(std::move(processor));
   if (error IS ERR::Okay) Self->Processor = pointer;
   else {
      std::lock_guard mixer_lock(*chain->Mutex);
      Self->Branches.clear();
   }

   return error;
}

/*********************************************************************************************************************

-FIELD-
LatencyBudget: The fixed delay, in milliseconds, within which the latencies of all branches are aligned.

Every branch is delayed so that its output emerges exactly LatencyBudget milliseconds after its input, rounded up to
whole frames at the output rate, regardless of the latency of the effects in the branch.  This keeps the branches
aligned when they are summed, and is reported as the splitter's @AudioEffect.Latency.

The budget must be at least the greatest combined latency of the effects in any one branch, including bypassed
effects.  For example, a branch containing an @AudioLimiter, which has a 5 ms lookahead, requires a budget of at least
5 ms.  With the default of zero, only effects without latency can be attached to a branch.

The value ranges from 0 to 100 and can only be set before initialisation.

*********************************************************************************************************************/

static ERR AUDIOSPLITTER_GET_LatencyBudget(extAudioSplitter *Self, double *Value)
{
   *Value = Self->LatencyBudget;
   return ERR::Okay;
}

static ERR AUDIOSPLITTER_SET_LatencyBudget(extAudioSplitter *Self, double Value)
{
   if (Self->initialised()) return ERR::Immutable;
   if ((not std::isfinite(Value)) or (Value < 0) or (Value > 100)) return ERR::OutOfRange;
   Self->LatencyBudget = Value;
   return ERR::Okay;
}

//********************************************************************************************************************

#include "class_audiosplitter_def.c"

static const FieldArray clAudioSplitterFields[] = {
   { "LatencyBudget", FDF_VIRTUAL|FDF_DOUBLE|FDF_RI, AUDIOSPLITTER_GET_LatencyBudget, AUDIOSPLITTER_SET_LatencyBudget },
   END_FIELD
};

//********************************************************************************************************************

static ERR add_audiosplitter_class()
{
   clAudioSplitter = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::AUDIOEFFECT),
      fl::ClassID(CLASSID::AUDIOSPLITTER),
      fl::Name("AudioSplitter"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clAudioSplitterActions),
      fl::Fields(clAudioSplitterFields),
      fl::Size(sizeof(extAudioSplitter)),
      fl::Path(MOD_PATH));
   return clAudioSplitter ? ERR::Okay : ERR::AddClass;
}
