#include <kotuku/main.h>

#include "package_identity.h"

#include <limits>

namespace tiri {

std::strong_ordering ParsedPackageVersion::operator<=>(const ParsedPackageVersion &Other) const noexcept
{
   const size_t shared = Count < Other.Count ? Count : Other.Count;
   for (size_t i = 0; i < shared; ++i) {
      if (Components[i] < Other.Components[i]) return std::strong_ordering::less;
      if (Components[i] > Other.Components[i]) return std::strong_ordering::greater;
   }
   return Count <=> Other.Count;
}

PackageValidationError validate_package_name(std::string_view Name) noexcept
{
   if (Name.empty()) return PackageValidationError::EMPTY;
   if (Name.size() > MAX_PACKAGE_NAME_LENGTH) return PackageValidationError::TOTAL_LENGTH;

   size_t component_start = 0;
   bool previous_dash = false;
   for (size_t i = 0; i <= Name.size(); ++i) {
      if (i IS Name.size() or Name[i] IS '/') {
         const size_t length = i - component_start;
         if (length IS 0) return PackageValidationError::EMPTY_COMPONENT;
         if (length > MAX_PACKAGE_NAME_COMPONENT_LENGTH) return PackageValidationError::COMPONENT_LENGTH;
         if (Name[component_start] IS '-') return PackageValidationError::LEADING_DASH;
         if (Name[i - 1] IS '-') return PackageValidationError::TRAILING_DASH;
         component_start = i + 1;
         previous_dash = false;
         continue;
      }

      const char value = Name[i];
      if (value IS '-') {
         if (previous_dash) return PackageValidationError::REPEATED_DASH;
         previous_dash = true;
      }
      else {
         previous_dash = false;
         if (not ((value >= 'a' and value <= 'z') or (value >= '0' and value <= '9'))) {
            return PackageValidationError::INVALID_CHARACTER;
         }
      }
   }
   return PackageValidationError::OKAY;
}

PackageValidationError parse_package_version(std::string_view Version, ParsedPackageVersion *Parsed) noexcept
{
   if (Version.empty()) return PackageValidationError::EMPTY;
   if (Version.size() > MAX_PACKAGE_VERSION_LENGTH) return PackageValidationError::TOTAL_LENGTH;

   ParsedPackageVersion result;
   size_t start = 0;
   while (start <= Version.size()) {
      if (result.Count >= MAX_PACKAGE_VERSION_COMPONENTS) return PackageValidationError::COMPONENT_COUNT;
      const size_t stop = Version.find('.', start);
      const size_t end = stop IS std::string_view::npos ? Version.size() : stop;
      if (end IS start) return PackageValidationError::EMPTY_COMPONENT;
      if (end - start > 1 and Version[start] IS '0') return PackageValidationError::LEADING_ZERO;

      uint32_t component = 0;
      for (size_t i = start; i < end; ++i) {
         const char value = Version[i];
         if (value < '0' or value > '9') return PackageValidationError::INVALID_CHARACTER;
         const uint32_t digit = uint32_t(value - '0');
         if (component > ((std::numeric_limits<uint32_t>::max)() - digit) / 10) {
            return PackageValidationError::COMPONENT_OVERFLOW;
         }
         component = component * 10 + digit;
      }
      result.Components[result.Count++] = component;
      if (stop IS std::string_view::npos) break;
      start = stop + 1;
   }

   if (Parsed) *Parsed = result;
   return PackageValidationError::OKAY;
}

PackageValidationError validate_package_identity(
   const PackageIdentity &Identity, ParsedPackageVersion *Parsed) noexcept
{
   if (auto error = validate_package_name(Identity.Name); error != PackageValidationError::OKAY) return error;
   return parse_package_version(Identity.Version, Parsed);
}

std::string_view package_validation_error_text(PackageValidationError Error) noexcept
{
   switch (Error) {
      case PackageValidationError::OKAY: return "valid";
      case PackageValidationError::EMPTY: return "must not be empty";
      case PackageValidationError::TOTAL_LENGTH: return "exceeds the maximum length";
      case PackageValidationError::COMPONENT_LENGTH: return "contains a component that exceeds the maximum length";
      case PackageValidationError::EMPTY_COMPONENT: return "contains an empty component";
      case PackageValidationError::INVALID_CHARACTER: return "contains an invalid character";
      case PackageValidationError::LEADING_DASH: return "contains a component with a leading dash";
      case PackageValidationError::TRAILING_DASH: return "contains a component with a trailing dash";
      case PackageValidationError::REPEATED_DASH: return "contains repeated dashes";
      case PackageValidationError::LEADING_ZERO: return "contains a non-canonical leading zero";
      case PackageValidationError::COMPONENT_COUNT: return "has too many components";
      case PackageValidationError::COMPONENT_OVERFLOW: return "contains a component larger than uint32";
   }
   return "is invalid";
}

} // namespace tiri
