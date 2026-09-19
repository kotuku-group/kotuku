#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tiri {

inline constexpr size_t MAX_VERSION_LENGTH = 255;
inline constexpr size_t MAX_VERSION_COMPONENTS = 16;
inline constexpr size_t MAX_CONSTRAINT_LENGTH = 1024;
inline constexpr size_t MAX_CONSTRAINT_COMPARISONS = 32;
inline constexpr size_t MAX_COMPATIBILITY_RECORDS = 4096;
inline constexpr size_t MAX_COMPATIBILITY_OWNER_LENGTH = 64 * 1024;
inline constexpr uint8_t COMPATIBILITY_SCHEMA_VERSION = 1;

enum class VersionError : uint8_t {
   Okay, Empty, Length, EmptyComponent, LeadingZero, InvalidCharacter, ComponentCount, ComponentOverflow,
   MissingVersion, InvalidOperator, MissingSeparator, ComparisonCount, InvalidMetadata, Truncated, TrailingData
};

struct VersionParseError {
   VersionError Error = VersionError::Okay;
   size_t Offset = 0;
   [[nodiscard]] explicit operator bool() const noexcept { return Error != VersionError::Okay; }
};

struct Version {
   std::string Text;
   std::vector<uint32_t> Components;
   [[nodiscard]] int compare(const Version &Other) const noexcept;
   [[nodiscard]] bool operator==(const Version &) const = default;
};

enum class VersionOperator : uint8_t { Exact = 1, Less, LessEqual, Greater, GreaterEqual };

struct VersionComparison {
   VersionOperator Operator = VersionOperator::Exact;
   Version Required;
   [[nodiscard]] bool operator==(const VersionComparison &) const = default;
};

struct VersionConstraint {
   std::string Original;
   std::vector<VersionComparison> Comparisons;
   [[nodiscard]] bool operator==(const VersionConstraint &) const = default;
};

struct DependencyRequirements {
   std::optional<VersionConstraint> Tiri;
   std::optional<VersionConstraint> Kotuku;
   [[nodiscard]] bool empty() const noexcept { return not Tiri and not Kotuku; }
   [[nodiscard]] bool operator==(const DependencyRequirements &) const = default;
};

struct CompatibilityRecord {
   std::string Owner;
   DependencyRequirements Requirements;
   [[nodiscard]] bool operator==(const CompatibilityRecord &) const = default;
};

struct RuntimeVersions { Version Tiri; Version Kotuku; };

struct CompatibilityFailure {
   std::string_view Domain;
   const VersionConstraint *Constraint = nullptr;
   const VersionComparison *Comparison = nullptr;
   const Version *Running = nullptr;
   std::string_view Owner;
};

[[nodiscard]] VersionParseError parse_version(std::string_view Text, Version &Output) noexcept;
[[nodiscard]] VersionParseError parse_version_constraint(std::string_view Text, VersionConstraint &Output) noexcept;
[[nodiscard]] std::string_view version_error_text(VersionError Error) noexcept;
[[nodiscard]] std::string comparison_text(const VersionComparison &Comparison);
[[nodiscard]] bool satisfies(const Version &Running, const VersionComparison &Comparison) noexcept;
[[nodiscard]] bool satisfies(const Version &Running, const VersionConstraint &Constraint,
   const VersionComparison **Failed = nullptr) noexcept;
[[nodiscard]] const RuntimeVersions &runtime_versions() noexcept;
[[nodiscard]] bool check_requirements(const DependencyRequirements &Requirements, const RuntimeVersions &Runtime,
   CompatibilityFailure *Failure = nullptr, std::string_view Owner = {}) noexcept;
[[nodiscard]] bool check_compatibility_records(std::span<const CompatibilityRecord> Records,
   const RuntimeVersions &Runtime, CompatibilityFailure *Failure = nullptr) noexcept;
[[nodiscard]] VersionParseError encode_compatibility_manifest(
   std::span<const CompatibilityRecord> Records, std::string &Output);
[[nodiscard]] VersionParseError decode_compatibility_manifest(
   std::string_view Input, std::vector<CompatibilityRecord> &Output);

} // namespace tiri
