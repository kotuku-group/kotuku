
#include <kotuku/main.h>

#include "version_constraints.h"
#include "runtime_versions.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <limits>
#include <tuple>

namespace tiri {
namespace {

//********************************************************************************************************************
// Returns whether a character is ASCII whitespace accepted between version comparisons.

[[nodiscard]] bool ascii_space(char Value) noexcept
{
   return Value IS ' ' or Value IS '\t' or Value IS '\n' or Value IS '\r' or Value IS '\f' or Value IS '\v';
}

//********************************************************************************************************************
// Appends an unsigned integer to a byte stream using ULEB128 encoding.

void append_uleb(std::string &Output, uint32_t Value)
{
   do {
      uint8_t byte = uint8_t(Value & 0x7f);
      Value >>= 7;
      if (Value) byte |= 0x80;
      Output.push_back(char(byte));
   } while (Value);
}

//********************************************************************************************************************
// Reads a ULEB128-encoded unsigned integer and advances the stream position.

[[nodiscard]] bool read_uleb(std::string_view Input, size_t &Position, uint32_t &Value) noexcept
{
   Value = 0;
   for (uint32_t shift = 0; shift < 35; shift += 7) {
      if (Position >= Input.size()) return false;
      const uint8_t byte = uint8_t(Input[Position++]);
      if (shift IS 28 and (byte & 0xf0)) return false;
      Value |= uint32_t(byte & 0x7f) << shift;
      if (not (byte & 0x80)) return true;
   }
   return false;
}

//********************************************************************************************************************
// Writes a version constraint in the manifest's compact binary representation.

void encode_constraint(std::string &Output, const VersionConstraint &Constraint)
{
   append_uleb(Output, uint32_t(Constraint.Comparisons.size()));
   for (const auto &comparison : Constraint.Comparisons) {
      Output.push_back(char(comparison.Operator));
      append_uleb(Output, uint32_t(comparison.Required.Text.size()));
      Output.append(comparison.Required.Text);
   }
}

//********************************************************************************************************************
// Reads and validates one version constraint from a binary compatibility manifest.

VersionParseError decode_constraint(std::string_view Input, size_t &Position, VersionConstraint &Output) noexcept
{
   const size_t constraint_start = Position;
   uint32_t count = 0;

   if (not read_uleb(Input, Position, count)) return { VersionError::Truncated, Position };
   if (count IS 0 or count > MAX_CONSTRAINT_COMPARISONS) return { VersionError::ComparisonCount, Position };

   Output = {};
   Output.Comparisons.reserve(count);

   for (uint32_t i = 0; i < count; ++i) {
      if (Position >= Input.size()) return { VersionError::Truncated, Position };
      const auto operation = VersionOperator(uint8_t(Input[Position++]));
      if (operation < VersionOperator::Exact or operation > VersionOperator::GreaterEqual) {
         return { VersionError::InvalidOperator, Position - 1 };
      }

      uint32_t size = 0;
      if (not read_uleb(Input, Position, size) or size > MAX_VERSION_LENGTH or size > Input.size() - Position) {
         return { VersionError::Truncated, Position };
      }

      std::string_view text = Input.substr(Position, size);
      Position += size;
      Version version;
      if (auto error = parse_version(text, version); error) {
         error.Offset += Position - size;
         return error;
      }

      Output.Comparisons.push_back({ operation, std::move(version) });
      if (Position - constraint_start > MAX_CONSTRAINT_LENGTH) return { VersionError::Length, constraint_start };
   }

   for (const auto &comparison : Output.Comparisons) {
      if (not Output.Original.empty()) Output.Original.push_back(' ');
      Output.Original += comparison_text(comparison);
   }
   return {};
}

//********************************************************************************************************************
// Produces the canonical binary form of a record's dependency requirements.

[[nodiscard]] std::string canonical_requirement_bytes(const DependencyRequirements &Requirements)
{
   std::string result;
   uint8_t flags = 0;
   if (Requirements.Tiri) flags |= 1;
   if (Requirements.Kotuku) flags |= 2;
   result.push_back(char(flags));
   if (Requirements.Tiri) encode_constraint(result, *Requirements.Tiri);
   if (Requirements.Kotuku) encode_constraint(result, *Requirements.Kotuku);
   return result;
}

} // namespace

//********************************************************************************************************************
// Compares this version with another version by their numeric components.

int Version::compare(const Version &Other) const noexcept
{
   const size_t shared = std::min(Components.size(), Other.Components.size());

   for (size_t i = 0; i < shared; ++i) {
      if (Components[i] < Other.Components[i]) return -1;
      if (Components[i] > Other.Components[i]) return 1;
   }

   if (Components.size() < Other.Components.size()) return -1;
   if (Components.size() > Other.Components.size()) return 1;

   return 0;
}

//********************************************************************************************************************
// Parses a dot-separated numeric version string into its canonical component form.

VersionParseError parse_version(std::string_view Text, Version &Output) noexcept
{
   Output = {};

   if (Text.empty()) return { VersionError::Empty, 0 };
   if (Text.size() > MAX_VERSION_LENGTH) return { VersionError::Length, MAX_VERSION_LENGTH };

   size_t start = 0;
   while (start <= Text.size()) {
      if (Output.Components.size() >= MAX_VERSION_COMPONENTS) return { VersionError::ComponentCount, start };

      const size_t found = Text.find('.', start);
      const size_t end = found IS std::string_view::npos ? Text.size() : found;

      if (end IS start) return { VersionError::EmptyComponent, start };
      if (end - start > 1 and Text[start] IS '0') return { VersionError::LeadingZero, start };

      uint32_t component = 0;
      for (size_t i = start; i < end; ++i) {
         if (Text[i] < '0' or Text[i] > '9') return { VersionError::InvalidCharacter, i };
         const uint32_t digit = uint32_t(Text[i] - '0');
         if (component > ((std::numeric_limits<uint32_t>::max)() - digit) / 10) {
            return { VersionError::ComponentOverflow, i };
         }
         component = component * 10 + digit;
      }

      Output.Components.push_back(component);
      if (found IS std::string_view::npos) break;
      start = found + 1;
   }

   Output.Text.assign(Text);
   return {};
}

//********************************************************************************************************************
// Parses a whitespace-separated set of version comparisons into a constraint.

VersionParseError parse_version_constraint(std::string_view Text, VersionConstraint &Output) noexcept
{
   Output = {};

   if (Text.size() > MAX_CONSTRAINT_LENGTH) return { VersionError::Length, MAX_CONSTRAINT_LENGTH };

   size_t position = 0;
   while (position < Text.size() and ascii_space(Text[position])) position++;
   if (position IS Text.size()) return { VersionError::Empty, position };

   while (position < Text.size()) {
      if (Output.Comparisons.size() >= MAX_CONSTRAINT_COMPARISONS) {
         return { VersionError::ComparisonCount, position };
      }

      VersionOperator operation = VersionOperator::Exact;
      if (Text[position] IS '<' or Text[position] IS '>') {
         const bool less = Text[position] IS '<';
         position++;
         const bool equal = position < Text.size() and Text[position] IS '=';
         if (equal) position++;
         operation = less ? (equal ? VersionOperator::LessEqual : VersionOperator::Less) :
            (equal ? VersionOperator::GreaterEqual : VersionOperator::Greater);
      }
      else if (Text[position] IS '=' or Text[position] IS '!') {
         return { VersionError::InvalidOperator, position };
      }

      if (position >= Text.size() or ascii_space(Text[position])) return { VersionError::MissingVersion, position };
      const size_t start = position;
      while (position < Text.size() and not ascii_space(Text[position])) position++;

      Version version;
      if (auto error = parse_version(Text.substr(start, position - start), version); error) {
         error.Offset += start;
         return error;
      }

      Output.Comparisons.push_back({ operation, std::move(version) });
      if (position IS Text.size()) break;
      while (position < Text.size() and ascii_space(Text[position])) position++;
   }
   Output.Original.assign(Text);
   return {};
}

//********************************************************************************************************************
// Returns a human-readable explanation for a version parsing error.

std::string_view version_error_text(VersionError Error) noexcept
{
   switch (Error) {
      case VersionError::Okay:              return "valid";
      case VersionError::Empty:             return "must not be empty";
      case VersionError::Length:            return "exceeds the maximum length";
      case VersionError::EmptyComponent:    return "contains an empty component";
      case VersionError::LeadingZero:       return "contains a non-canonical leading zero";
      case VersionError::InvalidCharacter:  return "contains an invalid character";
      case VersionError::ComponentCount:    return "has too many components";
      case VersionError::ComponentOverflow: return "contains a component larger than uint32";
      case VersionError::MissingVersion:    return "has an operator without a version";
      case VersionError::InvalidOperator:   return "contains an unsupported operator";
      case VersionError::MissingSeparator:  return "requires ASCII whitespace between comparisons";
      case VersionError::ComparisonCount:   return "has an invalid number of comparisons";
      case VersionError::InvalidMetadata:   return "contains invalid compatibility metadata";
      case VersionError::Truncated:         return "contains truncated compatibility metadata";
      case VersionError::TrailingData:      return "contains trailing compatibility metadata";
   }

   return "is invalid";
}

//********************************************************************************************************************
// Formats a version comparison as the canonical constraint-text fragment.

std::string comparison_text(const VersionComparison &Comparison)
{
   std::string result;

   switch (Comparison.Operator) {
      case VersionOperator::Exact:        break;
      case VersionOperator::Less:         result = "<"; break;
      case VersionOperator::LessEqual:    result = "<="; break;
      case VersionOperator::Greater:      result = ">"; break;
      case VersionOperator::GreaterEqual: result = ">="; break;
   }

   result += Comparison.Required.Text;
   return result;
}

//********************************************************************************************************************
// Determines whether a running version meets one version comparison.

bool satisfies(const Version &Running, const VersionComparison &Comparison) noexcept
{
   const int order = Running.compare(Comparison.Required);
   switch (Comparison.Operator) {
      case VersionOperator::Exact:        return order IS 0;
      case VersionOperator::Less:         return order < 0;
      case VersionOperator::LessEqual:    return order <= 0;
      case VersionOperator::Greater:      return order > 0;
      case VersionOperator::GreaterEqual: return order >= 0;
   }

   return false;
}

//********************************************************************************************************************
// Determines whether a running version meets every comparison in a constraint.

bool satisfies(const Version &Running, const VersionConstraint &Constraint,
   const VersionComparison **Failed) noexcept
{
   if (Failed) *Failed = nullptr;

   for (const auto &comparison : Constraint.Comparisons) {
      if (not satisfies(Running, comparison)) {
         if (Failed) *Failed = &comparison;
         return false;
      }
   }

   return true;
}

//********************************************************************************************************************
// Determines whether a running Tiri version meets its language-contract constraint.  An unprefixed requirement
// accepts later versions within the same major language version; explicit comparison operators retain their normal
// numeric meanings.

static bool satisfies_tiri(const Version &Running, const VersionConstraint &Constraint,
   const VersionComparison **Failed) noexcept
{
   if (Failed) *Failed = nullptr;

   for (const auto &comparison : Constraint.Comparisons) {
      bool compatible;
      if (comparison.Operator IS VersionOperator::Exact) {
         compatible = not Running.Components.empty() and not comparison.Required.Components.empty() and
            Running.Components.front() IS comparison.Required.Components.front() and
            Running.compare(comparison.Required) >= 0;
      }
      else compatible = satisfies(Running, comparison);

      if (not compatible) {
         if (Failed) *Failed = &comparison;
         return false;
      }
   }

   return true;
}

//********************************************************************************************************************
// Returns the parsed Tiri and Kōtuku versions of the running environment.

const RuntimeVersions &runtime_versions() noexcept
{
   static const RuntimeVersions versions = [] {
      RuntimeVersions result;
      if (parse_version(TIRI_LANGUAGE_VERSION, result.Tiri) or
          parse_version(KOTUKU_RUNTIME_VERSION, result.Kotuku)) std::abort();
      return result;
   }();

   return versions;
}

//********************************************************************************************************************
// Checks dependency requirements against runtime versions and records the first failure.

bool check_requirements(const DependencyRequirements &Requirements, const RuntimeVersions &Runtime,
   CompatibilityFailure *Failure, std::string_view Owner) noexcept
{
   if (Failure) *Failure = {};
   const VersionComparison *failed = nullptr;

   if (Requirements.Tiri and not satisfies_tiri(Runtime.Tiri, *Requirements.Tiri, &failed)) {
      if (Failure) *Failure = { "tiri", &*Requirements.Tiri, failed, &Runtime.Tiri, Owner };
      return false;
   }

   if (Requirements.Kotuku and not satisfies(Runtime.Kotuku, *Requirements.Kotuku, &failed)) {
      if (Failure) *Failure = { "kotuku", &*Requirements.Kotuku, failed, &Runtime.Kotuku, Owner };
      return false;
   }
   return true;
}

//********************************************************************************************************************
// Checks every compatibility record against runtime versions until one fails.

bool check_compatibility_records(std::span<const CompatibilityRecord> Records,
   const RuntimeVersions &Runtime, CompatibilityFailure *Failure) noexcept
{
   for (const auto &record : Records) {
      if (not check_requirements(record.Requirements, Runtime, Failure, record.Owner)) return false;
   }
   return true;
}

//********************************************************************************************************************
// Checks concrete package metadata against one versioned import edge.  The public import name does not imply package
// ownership; a package resolver supplies and validates the expected package identity separately.

bool check_package_import(const PackageImportRequirement &Requirement,
   const std::optional<PackageIdentity> &Identity, PackageImportFailure *Failure) noexcept
{
   if (Failure) *Failure = {};
   if (not Identity) {
      if (Failure) Failure->Kind = PackageImportFailureKind::MissingMetadata;
      return false;
   }

   Version loaded;
   if (validate_package_name(Identity->Name) != PackageValidationError::OKAY or
       parse_version(Identity->Version, loaded)) {
      if (Failure) Failure->Kind = PackageImportFailureKind::InvalidMetadata;
      return false;
   }

   const VersionComparison *failed = nullptr;
   if (not satisfies(loaded, Requirement.Constraint, &failed)) {
      if (Failure) {
         Failure->Kind = PackageImportFailureKind::Unsatisfied;
         Failure->Comparison = failed;
         Failure->LoadedVersion = std::move(loaded);
      }
      return false;
   }
   return true;
}

//********************************************************************************************************************
// Formats the stable user-facing diagnostic for a failed versioned import check.

std::string package_import_error(const PackageImportRequirement &Requirement, const PackageImportFailure &Failure)
{
   switch (Failure.Kind) {
      case PackageImportFailureKind::MissingMetadata:
         return std::format("versioned import '{}' requires @Package metadata", Requirement.ImportName);
      case PackageImportFailureKind::InvalidMetadata:
         return std::format("versioned import '{}' loaded invalid package metadata", Requirement.ImportName);
      case PackageImportFailureKind::Unsatisfied:
         return std::format("versioned import '{}' requires '{}', but loaded version {} fails comparison {}",
            Requirement.ImportName, Requirement.Constraint.Original, Failure.LoadedVersion.Text,
            Failure.Comparison ? comparison_text(*Failure.Comparison) : std::string("<unknown>"));
      case PackageImportFailureKind::None:
         break;
   }
   return std::format("versioned import '{}' failed package validation", Requirement.ImportName);
}

//********************************************************************************************************************
// Validates, canonicalises and serialises compatibility records into a binary manifest.

VersionParseError encode_compatibility_manifest(std::span<const CompatibilityRecord> Records, std::string &Output)
{
   Output.clear();
   if (Records.size() > MAX_COMPATIBILITY_RECORDS) return { VersionError::ComparisonCount, 0 };
   std::vector<std::pair<CompatibilityRecord, std::string>> canonical;
   canonical.reserve(Records.size());

   for (const auto &record : Records) {
      if (record.Owner.empty() or record.Owner.size() > MAX_COMPATIBILITY_OWNER_LENGTH or
          record.Owner.find('\0') != std::string::npos) return { VersionError::InvalidMetadata, 0 };
      const std::string requirements = canonical_requirement_bytes(record.Requirements);

      if ((record.Requirements.Tiri and (record.Requirements.Tiri->Comparisons.empty() or
           record.Requirements.Tiri->Comparisons.size() > MAX_CONSTRAINT_COMPARISONS or
           record.Requirements.Tiri->Original.size() > MAX_CONSTRAINT_LENGTH)) or
          (record.Requirements.Kotuku and (record.Requirements.Kotuku->Comparisons.empty() or
           record.Requirements.Kotuku->Comparisons.size() > MAX_CONSTRAINT_COMPARISONS or
           record.Requirements.Kotuku->Original.size() > MAX_CONSTRAINT_LENGTH))) {
         return { VersionError::ComparisonCount, 0 };
      }

      auto valid_constraint = [](const std::optional<VersionConstraint> &Constraint) {
         if (not Constraint) return true;
         for (const auto &comparison : Constraint->Comparisons) {
            Version parsed;
            if (parse_version(comparison.Required.Text, parsed) or parsed != comparison.Required) return false;
         }
         return true;
      };

      if (not valid_constraint(record.Requirements.Tiri) or
          not valid_constraint(record.Requirements.Kotuku)) return { VersionError::InvalidMetadata, 0 };
      canonical.emplace_back(record, requirements);
   }

   std::ranges::sort(canonical, [](const auto &Left, const auto &Right) {
      return std::tie(Left.first.Owner, Left.second) < std::tie(Right.first.Owner, Right.second);
   });

   canonical.erase(std::unique(canonical.begin(), canonical.end(), [](const auto &Left, const auto &Right) {
      return Left.first.Owner IS Right.first.Owner and Left.second IS Right.second;
   }), canonical.end());

   Output.push_back(char(COMPATIBILITY_SCHEMA_VERSION));
   append_uleb(Output, uint32_t(canonical.size()));
   for (const auto &[record, requirements] : canonical) {
      append_uleb(Output, uint32_t(record.Owner.size()));
      Output.append(record.Owner);
      Output.append(requirements);
   }
   return {};
}

//********************************************************************************************************************
// Decodes and validates a canonical binary compatibility manifest.

VersionParseError decode_compatibility_manifest(std::string_view Input, std::vector<CompatibilityRecord> &Output)
{
   Output.clear();
   size_t position = 0;

   if (Input.empty()) return { VersionError::Truncated, 0 };
   if (uint8_t(Input[position++]) != COMPATIBILITY_SCHEMA_VERSION) return { VersionError::InvalidMetadata, 0 };
   uint32_t count = 0;

   if (not read_uleb(Input, position, count)) return { VersionError::Truncated, position };
   if (count > MAX_COMPATIBILITY_RECORDS) return { VersionError::ComparisonCount, position };

   Output.reserve(count);
   std::string previous_owner;
   std::string previous_requirements;

   for (uint32_t i = 0; i < count; ++i) {
      uint32_t owner_size = 0;
      if (not read_uleb(Input, position, owner_size) or owner_size IS 0 or
          owner_size > MAX_COMPATIBILITY_OWNER_LENGTH or owner_size > Input.size() - position) {
         return { VersionError::Truncated, position };
      }

      CompatibilityRecord record;
      record.Owner.assign(Input.substr(position, owner_size));
      position += owner_size;

      if (record.Owner.find('\0') != std::string::npos) return { VersionError::InvalidMetadata, position - owner_size };
      const size_t requirements_start = position;
      if (position >= Input.size()) return { VersionError::Truncated, position };

      const uint8_t flags = uint8_t(Input[position++]);
      if (flags & ~uint8_t(3)) return { VersionError::InvalidMetadata, position - 1 };

      if (flags & 1) {
         VersionConstraint constraint;
         if (auto error = decode_constraint(Input, position, constraint); error) return error;
         record.Requirements.Tiri = std::move(constraint);
      }

      if (flags & 2) {
         VersionConstraint constraint;
         if (auto error = decode_constraint(Input, position, constraint); error) return error;
         record.Requirements.Kotuku = std::move(constraint);
      }

      const std::string requirements(Input.substr(requirements_start, position - requirements_start));
      if (not previous_owner.empty() and
          (record.Owner < previous_owner or
             (record.Owner IS previous_owner and requirements <= previous_requirements))) {
         return { VersionError::InvalidMetadata, requirements_start };
      }

      previous_owner = record.Owner;
      previous_requirements = requirements;
      Output.push_back(std::move(record));
   }

   if (position != Input.size()) return { VersionError::TrailingData, position };
   return {};
}

} // namespace tiri
