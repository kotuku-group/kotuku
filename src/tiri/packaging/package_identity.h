#pragma once

#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace tiri {

inline constexpr size_t MAX_PACKAGE_NAME_LENGTH = 255;
inline constexpr size_t MAX_PACKAGE_NAME_COMPONENT_LENGTH = 63;
inline constexpr size_t MAX_PACKAGE_VERSION_LENGTH = 255;
inline constexpr size_t MAX_PACKAGE_VERSION_COMPONENTS = 16;

enum class PackageValidationError : uint8_t {
   OKAY,
   EMPTY,
   TOTAL_LENGTH,
   COMPONENT_LENGTH,
   EMPTY_COMPONENT,
   INVALID_CHARACTER,
   LEADING_DASH,
   TRAILING_DASH,
   REPEATED_DASH,
   LEADING_ZERO,
   COMPONENT_COUNT,
   COMPONENT_OVERFLOW
};

struct ParsedPackageVersion {
   std::array<uint32_t, MAX_PACKAGE_VERSION_COMPONENTS> Components = {};
   uint8_t Count = 0;

   [[nodiscard]] std::strong_ordering operator<=>(const ParsedPackageVersion &Other) const noexcept;
   [[nodiscard]] bool operator==(const ParsedPackageVersion &Other) const noexcept = default;
};

struct PackageIdentity {
   std::string Name;
   std::string Version;

   [[nodiscard]] bool empty() const noexcept { return Name.empty() and Version.empty(); }
   [[nodiscard]] explicit operator bool() const noexcept { return not empty(); }
   [[nodiscard]] bool operator==(const PackageIdentity &) const = default;
};

[[nodiscard]] PackageValidationError validate_package_name(std::string_view Name) noexcept;
[[nodiscard]] PackageValidationError parse_package_version(
   std::string_view Version, ParsedPackageVersion *Parsed = nullptr) noexcept;
[[nodiscard]] PackageValidationError validate_package_identity(
   const PackageIdentity &Identity, ParsedPackageVersion *Parsed = nullptr) noexcept;
[[nodiscard]] std::string_view package_validation_error_text(PackageValidationError Error) noexcept;

} // namespace tiri
