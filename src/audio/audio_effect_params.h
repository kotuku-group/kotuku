// Parameter descriptors for AudioEffect subclasses.  A single static table per class drives value validation,
// generic path-based access and the published XML schema, so the published limits always match the enforced ones.

#pragma once

#include <cmath>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class extAudioEffect;

enum class APT : int8_t { DOUBLE, INT, ENUM, BOOL };             // Parameter type
enum class APU : int8_t { NONE, HZ, DB, PERCENT, MS, RATIO };    // Unit
enum class APS : int8_t { LINEAR, LOG };                         // Display scale
enum class APB : int8_t { FIXED, NYQUIST };                      // Maximum bound resolution

// A member is ignored, or its maximum lowered, when the sibling named by Key holds one of Values.

struct AudioParamRule {
   CSTRING Key;
   std::span<const int> Values;
   bool Inactive = false;  // True if the member is ignored by the DSP when the rule matches
   double CapMax = 0;      // Otherwise the member's maximum when the rule matches
};

struct AudioParamOption {
   int Value;
   CSTRING Key;
   CSTRING Label;
   CSTRING Description = nullptr;  // Optional sentences suitable for presentation to the user
};

struct AudioParamDesc {
   CSTRING Key;
   CSTRING Label;
   CSTRING Description = nullptr;            // Optional sentences suitable for presentation to the user
   APT Type = APT::DOUBLE;
   APU Unit = APU::NONE;
   APS Scale = APS::LINEAR;
   double Min = 0, Max = 0, Default = 0;
   double Step = 0;                          // Zero for continuous values
   bool MinExclusive = false;
   APB MaxBound = APB::FIXED;                // NYQUIST resolves to OutputRate * 0.5 and is exclusive
   std::span<const AudioParamOption> Options = {};
   std::span<const AudioParamRule> Rules = {};
};

struct AudioParamGroup {
   CSTRING Key;
   CSTRING Label;
   CSTRING Description = nullptr;
   int MinCount, MaxCount;
   std::span<const AudioParamDesc> Members;
};

// A snapshot of every parameter value.  Origin records the committed index an entry was copied from, or -1 for a new
// entry, so that DSP state can follow entries that move when other entries are inserted or removed.

struct AudioParamEntry {
   std::vector<double> Values;
   int Origin = -1;
};

struct AudioParamState {
   std::vector<double> Params;                       // One value per AudioEffectSchema::Params entry
   std::vector<std::vector<AudioParamEntry>> Groups; // One entry list per AudioEffectSchema::Groups entry
};

// Prepared outside the mixer lock.  Publish may only copy bounded DSP history and swap storage; destruction occurs
// after releasing the lock, including disposal of any storage retired by publication.

class AudioParamUpdate {
public:
   virtual ~AudioParamUpdate() = default;
   virtual void publish(extAudioEffect *Effect) = 0;
   // Latency-affecting edits must declare their prepared latency before publication; -1 preserves it.
   virtual int64_t latency() const { return -1; }
};

enum class AudioOutputKind { CURVE, SCALAR };

struct AudioOutputDesc {
   CSTRING Key;
   AudioOutputKind Kind = AudioOutputKind::CURVE;
   CSTRING Label = nullptr;
   CSTRING Description = nullptr;
   CSTRING Unit = nullptr;
   CSTRING Scope = nullptr;
   CSTRING Semantics = nullptr;
   int Slot = -1;
};

struct AudioEffectSchema {
   CSTRING ClassName;
   int Version;
   CSTRING Description = nullptr;
   std::span<const AudioParamDesc> Params;
   std::span<const AudioParamGroup> Groups;
   std::span<const AudioOutputDesc> Outputs;

   // Read() snapshots committed parameters.  Apply() handles pre-initialisation edits and is the fallback commit
   // path for classes without Prepare().  Prepare() builds a validated update outside the mixer lock; publish()
   // runs under that lock once its sample rate has been rechecked.  GroupCount() avoids snapshotting for counts.

   void (*Read)(extAudioEffect *, AudioParamState &);
   void (*Apply)(extAudioEffect *, const AudioParamState &);
   ERR (*Response)(extAudioEffect *, std::span<const double>, std::span<double>) = nullptr;
   std::unique_ptr<AudioParamUpdate> (*Prepare)(extAudioEffect *, const AudioParamState &, int) = nullptr;
   size_t (*GroupCount)(extAudioEffect *, size_t) = nullptr;
};

//********************************************************************************************************************
// Resolve the effective maximum for a parameter.  Returns false if the parameter has no applicable maximum.

inline bool param_maximum(const AudioParamDesc &Desc, int Rate, double &Max, bool &Exclusive)
{
   if (Desc.MaxBound IS APB::NYQUIST) {
      if (Rate <= 0) return false;
      Max = double(Rate) * 0.5;
      Exclusive = true;
   }
   else {
      Max = Desc.Max;
      Exclusive = false;
   }
   return true;
}

//********************************************************************************************************************
// Check a value against its own descriptor.  Rules that depend on sibling values are checked by check_rules().

inline ERR check_param(const AudioParamDesc &Desc, double Value, int Rate)
{
   if (not std::isfinite(Value)) return ERR::InvalidValue;

   if (Desc.Type != APT::DOUBLE) {
      if (Value != std::trunc(Value)) return ERR::InvalidValue;
      if (Desc.Type IS APT::ENUM) {
         for (const auto &option : Desc.Options) {
            if (double(option.Value) IS Value) return ERR::Okay;
         }
         return ERR::InvalidValue;
      }
   }

   if (Desc.MinExclusive ? (Value <= Desc.Min) : (Value < Desc.Min)) return ERR::InvalidValue;

   double max;
   bool exclusive;
   if (param_maximum(Desc, Rate, max, exclusive)) {
      if (exclusive ? (Value >= max) : (Value > max)) return ERR::InvalidValue;
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Check the rules of every member in a parameter set (the top-level parameters, or one group entry).

inline ERR check_rules(std::span<const AudioParamDesc> Members, std::span<const double> Values)
{
   for (size_t m = 0; m < Members.size(); m++) {
      for (const auto &rule : Members[m].Rules) {
         if (rule.Inactive) continue;

         for (size_t s = 0; s < Members.size(); s++) {
            if (std::string_view(Members[s].Key) != rule.Key) continue;
            for (auto v : rule.Values) {
               if ((double(v) IS Values[s]) and (Values[m] > rule.CapMax)) return ERR::InvalidValue;
            }
         }
      }
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Validate a complete state: shape, value ranges, group counts and rules.  If Committed is provided, values that are
// unchanged from their committed counterparts are not range-checked again, and rules are only checked for parameter
// sets that changed.  This allows edits after a change of output rate has moved a Nyquist bound below an existing
// value; the DSP clamps such values.

inline ERR validate_state(const AudioEffectSchema &Schema, const AudioParamState &State, int Rate,
   const AudioParamState *Committed = nullptr)
{
   if (State.Params.size() != Schema.Params.size()) return ERR::InvalidValue;
   if (State.Groups.size() != Schema.Groups.size()) return ERR::InvalidValue;
   if (Committed and ((Committed->Params.size() != State.Params.size()) or
       (Committed->Groups.size() != State.Groups.size()))) Committed = nullptr;

   bool changed = not Committed;
   for (size_t p = 0; p < Schema.Params.size(); p++) {
      if (Committed and (Committed->Params[p] IS State.Params[p])) continue;
      changed = true;
      if (auto error = check_param(Schema.Params[p], State.Params[p], Rate); error != ERR::Okay) return error;
   }
   if (changed) {
      if (auto error = check_rules(Schema.Params, State.Params); error != ERR::Okay) return error;
   }

   for (size_t g = 0; g < Schema.Groups.size(); g++) {
      const auto &group = Schema.Groups[g];
      const auto &entries = State.Groups[g];
      if ((int(entries.size()) < group.MinCount) or (int(entries.size()) > group.MaxCount)) return ERR::OutOfRange;

      for (const auto &entry : entries) {
         if (entry.Values.size() != group.Members.size()) return ERR::InvalidValue;

         const AudioParamEntry *origin = nullptr;
         if (Committed and (entry.Origin >= 0) and (size_t(entry.Origin) < Committed->Groups[g].size())) {
            origin = &Committed->Groups[g][entry.Origin];
            if (origin->Values.size() != entry.Values.size()) origin = nullptr;
         }

         bool entry_changed = not origin;
         for (size_t m = 0; m < group.Members.size(); m++) {
            if (origin and (origin->Values[m] IS entry.Values[m])) continue;
            entry_changed = true;
            if (auto error = check_param(group.Members[m], entry.Values[m], Rate); error != ERR::Okay) return error;
         }
         if (entry_changed) {
            if (auto error = check_rules(group.Members, entry.Values); error != ERR::Okay) return error;
         }
      }
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Default values for a new group entry.  A default at or above a Nyquist bound is moved to half of that bound.

inline AudioParamEntry default_entry(const AudioParamGroup &Group, int Rate)
{
   AudioParamEntry entry;
   entry.Values.reserve(Group.Members.size());
   for (const auto &member : Group.Members) {
      double value = member.Default;
      double max;
      bool exclusive;
      if ((member.MaxBound IS APB::NYQUIST) and param_maximum(member, Rate, max, exclusive) and (value >= max)) {
         value = max * 0.5;
      }
      entry.Values.push_back(value);
   }
   return entry;
}

//********************************************************************************************************************
// Resolve a group by key.  Returns -1 if the key is unknown.

inline int find_group(const AudioEffectSchema &Schema, std::string_view Key)
{
   for (size_t g = 0; g < Schema.Groups.size(); g++) {
      if (Key IS Schema.Groups[g].Key) return int(g);
   }
   return -1;
}

//********************************************************************************************************************
// Resolve a path of the form "key" or "group[index].key" to a value slot within State.  Returns ERR::Search if the
// path is malformed or names an unknown parameter, and ERR::OutOfRange if the index is not a current entry.

inline ERR resolve_path(const AudioEffectSchema &Schema, AudioParamState &State, std::string_view Path,
   double *&Value, const AudioParamDesc *&Desc)
{
   auto bracket = Path.find('[');
   if (bracket IS std::string_view::npos) {
      for (size_t p = 0; p < Schema.Params.size(); p++) {
         if (Path IS Schema.Params[p].Key) {
            Value = &State.Params[p];
            Desc = &Schema.Params[p];
            return ERR::Okay;
         }
      }
      return ERR::Search;
   }

   auto group_index = find_group(Schema, Path.substr(0, bracket));
   if (group_index < 0) return ERR::Search;

   auto close = Path.find(']', bracket);
   if ((close IS std::string_view::npos) or (close IS bracket + 1)) return ERR::Search;
   if ((close + 1 >= Path.size()) or (Path[close + 1] != '.')) return ERR::Search;

   int64_t index = 0;
   for (auto c : Path.substr(bracket + 1, close - bracket - 1)) {
      if ((c < '0') or (c > '9')) return ERR::Search;
      index = (index * 10) + (c - '0');
      if (index > 0x7fffffff) return ERR::OutOfRange;
   }

   const auto &group = Schema.Groups[group_index];
   auto key = Path.substr(close + 2);
   for (size_t m = 0; m < group.Members.size(); m++) {
      if (key IS group.Members[m].Key) {
         auto &entries = State.Groups[group_index];
         if (index >= int64_t(entries.size())) return ERR::OutOfRange;
         Value = &entries[index].Values[m];
         Desc = &group.Members[m];
         return ERR::Okay;
      }
   }
   return ERR::Search;
}

//********************************************************************************************************************
// Schema XML generation.

namespace schema_xml {

inline std::string escape(std::string_view Value)
{
   std::string out;
   out.reserve(Value.size());
   for (auto c : Value) {
      switch (c) {
         case '&': out += "&amp;"; break;
         case '<': out += "&lt;"; break;
         case '>': out += "&gt;"; break;
         case '"': out += "&quot;"; break;
         default:  out += c;
      }
   }
   return out;
}

inline CSTRING type_name(APT Type)
{
   switch (Type) {
      case APT::INT:  return "int";
      case APT::ENUM: return "enum";
      case APT::BOOL: return "bool";
      default:        return "double";
   }
}

inline CSTRING unit_name(APU Unit)
{
   switch (Unit) {
      case APU::HZ:      return "Hz";
      case APU::DB:      return "dB";
      case APU::PERCENT: return "%";
      case APU::MS:      return "ms";
      case APU::RATIO:   return "ratio";
      default:           return nullptr;
   }
}

//********************************************************************************************************************

inline void description(std::string &Out, CSTRING Description)
{
   if (Description and Description[0]) Out += std::format(" description=\"{}\"", escape(Description));
}

inline void param(std::string &Out, const AudioParamDesc &Desc, std::string_view Indent)
{
   Out += std::format("{}<param key=\"{}\" label=\"{}\"", Indent, escape(Desc.Key), escape(Desc.Label));
   description(Out, Desc.Description);
   Out += std::format(" type=\"{}\"", type_name(Desc.Type));
   if (auto unit = unit_name(Desc.Unit)) Out += std::format(" unit=\"{}\"", unit);
   if (Desc.Type != APT::ENUM) {
      Out += std::format(" scale=\"{}\"", (Desc.Scale IS APS::LOG) ? "log" : "linear");
      Out += std::format(" min=\"{}\"", Desc.Min);
      if (Desc.MinExclusive) Out += " min-exclusive=\"1\"";
      if (Desc.MaxBound IS APB::NYQUIST) Out += " max=\"nyquist\" max-exclusive=\"1\"";
      else Out += std::format(" max=\"{}\"", Desc.Max);
      if (Desc.Step > 0) Out += std::format(" step=\"{}\"", Desc.Step);
   }
   Out += std::format(" default=\"{}\"", Desc.Default);

   if (Desc.Options.empty() and Desc.Rules.empty()) {
      Out += "/>\n";
      return;
   }

   Out += ">\n";
   for (const auto &option : Desc.Options) {
      Out += std::format("{}  <option value=\"{}\" key=\"{}\" label=\"{}\"", Indent, option.Value,
         escape(option.Key), escape(option.Label));
      description(Out, option.Description);
      Out += "/>\n";
   }
   for (const auto &rule : Desc.Rules) {
      std::string values;
      for (auto v : rule.Values) {
         if (not values.empty()) values += ',';
         values += std::to_string(v);
      }
      Out += std::format("{}  <rule key=\"{}\" values=\"{}\"", Indent, escape(rule.Key), values);
      if (rule.Inactive) Out += " inactive=\"1\"/>\n";
      else Out += std::format(" max=\"{}\"/>\n", rule.CapMax);
   }
   Out += std::format("{}</param>\n", Indent);
}

} // namespace schema_xml

//********************************************************************************************************************

inline std::string build_schema_xml(const AudioEffectSchema &Schema, bool Stereo = true)
{
   std::string out = std::format("<effect class=\"{}\" version=\"{}\"", schema_xml::escape(Schema.ClassName),
      Schema.Version);
   schema_xml::description(out, Schema.Description);
   out += ">\n";
   for (const auto &desc : Schema.Params) schema_xml::param(out, desc, "  ");
   for (const auto &group : Schema.Groups) {
      out += std::format("  <group key=\"{}\" label=\"{}\"", schema_xml::escape(group.Key),
         schema_xml::escape(group.Label));
      schema_xml::description(out, group.Description);
      out += std::format(" min=\"{}\" max=\"{}\">\n", group.MinCount, group.MaxCount);
      for (const auto &desc : group.Members) schema_xml::param(out, desc, "    ");
      out += "  </group>\n";
   }
   for (const auto &output : Schema.Outputs) {
      if (!Stereo and output.Scope and std::string_view(output.Scope) IS "right") continue;
      out += std::format("  <output key=\"{}\" type=\"{}\"", schema_xml::escape(output.Key),
         output.Kind IS AudioOutputKind::CURVE ? "curve" : "scalar");
      if (output.Label) out += std::format(" label=\"{}\"", schema_xml::escape(output.Label));
      schema_xml::description(out, output.Description);
      if (output.Unit) out += std::format(" unit=\"{}\"", schema_xml::escape(output.Unit));
      if (output.Scope) out += std::format(" scope=\"{}\"", schema_xml::escape(output.Scope));
      if (output.Semantics) out += std::format(" semantics=\"{}\"", schema_xml::escape(output.Semantics));
      if (output.Slot >= 0) out += std::format(" slot=\"{}\"", output.Slot);
      out += "/>\n";
   }
   out += "</effect>\n";
   return out;
}
