#pragma once

#include <kotuku/modules/config.h>

#include "version_constraints.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tiri {

struct PackageCandidate {
   std::string Version;                // Version string from the package index.
   ParsedPackageVersion ParsedVersion; // Parsed version used to sort candidates.
   std::string RelativePath;           // Target path relative to the packages volume.
};

using PackageResolverSnapshot = std::unordered_map<std::string, std::vector<PackageCandidate>>; // Public import groups.

struct ImportResolutionRequest {
   std::string_view LogicalName;                          // Public name requested by the import.
   const PackageImportRequirement *Requirement = nullptr; // Optional version constraint for the import.
};

struct ResolvedImport {
   std::string LogicalName;                    // Public name used to request the import.
   std::string ResolvedPath;                   // Selected target path, resolved to a file when verified.
   std::optional<std::string> SelectedVersion; // Indexed version, absent for a local import.
   bool PackageManaged = false;                // Whether the target came from the package index.
};

enum class ImportResolutionError : uint8_t {
   Okay,
   InvalidName,
   UnavailableIndex,
   MissingGroup,
   UnsatisfiedConstraint,
   InvalidTarget,
   MissingTarget
};

struct ImportResolutionResult {
   ResolvedImport Import;
   ImportResolutionError Error = ImportResolutionError::Okay;
   std::string Detail;

   [[nodiscard]] explicit operator bool() const noexcept { return Error IS ImportResolutionError::Okay; }
};

[[nodiscard]] bool package_index_path_is_safe(std::string_view Path) noexcept;
[[nodiscard]] ImportResolutionError build_package_resolver_snapshot(
   const ConfigGroups &Groups, std::shared_ptr<const PackageResolverSnapshot> &Output, std::string &Diagnostic);
[[nodiscard]] ImportResolutionResult select_package_import(const PackageResolverSnapshot &Snapshot,
   const ImportResolutionRequest &Request);
[[nodiscard]] ImportResolutionResult resolve_package_import(const PackageResolverSnapshot &Snapshot,
   const ImportResolutionRequest &Request);
[[nodiscard]] ImportResolutionResult resolve_package_import(
   const ImportResolutionRequest &Request);
[[nodiscard]] std::string import_resolution_error(const ImportResolutionRequest &Request,
   const ImportResolutionResult &Result);
[[nodiscard]] bool check_resolved_package(const ResolvedImport &Import,
   const std::optional<PackageIdentity> &Declared, std::string &Diagnostic);

extern objConfig *glPackageIndex;
extern std::shared_ptr<const PackageResolverSnapshot> glPackageResolver;

} // namespace tiri
