#include <kotuku/main.h>
#include <kotuku/modules/config.h>
#include <kotuku/modules/filesystem.h>

#include "package_resolver.h"

#include <algorithm>
#include <format>
#include <tuple>

namespace tiri {

objConfig *glPackageIndex = nullptr;
std::shared_ptr<const PackageResolverSnapshot> glPackageResolver;

//********************************************************************************************************************
// Accept only relative package index paths made of non-empty, ordinary components.

bool package_index_path_is_safe(std::string_view Path) noexcept
{
   if (Path.empty() or Path.front() IS '/' or Path.find(':') != std::string_view::npos or
       Path.find('\\') != std::string_view::npos) return false;

   size_t start = 0;
   while (start <= Path.size()) {
      const size_t separator = Path.find('/', start);
      const std::string_view component = separator IS std::string_view::npos ? Path.substr(start) :
         Path.substr(start, separator - start);
      if (component.empty() or component IS "." or component IS "..") return false;
      if (separator IS std::string_view::npos) break;
      start = separator + 1;
   }
   return true;
}

//********************************************************************************************************************
// Validate the index groups and build a snapshot with each package's versions in ascending order.

ImportResolutionError build_package_resolver_snapshot(const ConfigGroups &Groups,
   std::shared_ptr<const PackageResolverSnapshot> &Output, std::string &Diagnostic)
{
   Output.reset();
   auto snapshot = std::make_shared<PackageResolverSnapshot>();

   for (const auto &[name, keys] : Groups) {
      if (name.empty()) continue;

      if (validate_package_name(name) != PackageValidationError::OKAY) {
         Diagnostic = std::format("package index group '{}' is not a valid public import name", name);
         return ImportResolutionError::InvalidName;
      }

      if (keys.empty()) {
         Diagnostic = std::format("package index group '{}' has no versions", name);
         return ImportResolutionError::UnavailableIndex;
      }

      auto inserted = snapshot->try_emplace(name);
      auto &candidates = inserted.first->second;
      candidates.reserve(keys.size());
      for (const auto &[version, path] : keys) {
         ParsedPackageVersion parsed;
         if (parse_package_version(version, &parsed) != PackageValidationError::OKAY) {
            Diagnostic = std::format("package index group '{}' contains invalid version '{}'", name, version);
            return ImportResolutionError::InvalidTarget;
         }

         if (not package_index_path_is_safe(path)) {
            Diagnostic = std::format("package index entry '{}@{}' contains unsafe path '{}'", name, version, path);
            return ImportResolutionError::InvalidTarget;
         }
         candidates.push_back({ version, parsed, path });
      }

      std::ranges::sort(candidates, [](const auto &Left, const auto &Right) {
         if (Left.ParsedVersion != Right.ParsedVersion) return Left.ParsedVersion < Right.ParsedVersion;
         return std::tie(Left.Version, Left.RelativePath) < std::tie(Right.Version, Right.RelativePath);
      });
   }

   if (snapshot->empty()) {
      Diagnostic = "package index contains no non-empty groups";
      return ImportResolutionError::UnavailableIndex;
   }

   Output = std::move(snapshot);
   Diagnostic.clear();
   return ImportResolutionError::Okay;
}

//********************************************************************************************************************
// Select the highest indexed version satisfying the request.

ImportResolutionResult select_package_import(const PackageResolverSnapshot &Snapshot,
   const ImportResolutionRequest &Request)
{
   ImportResolutionResult result;
   result.Import.LogicalName.assign(Request.LogicalName);
   if (validate_package_name(Request.LogicalName) != PackageValidationError::OKAY) {
      result.Error = ImportResolutionError::InvalidName;
      return result;
   }

   auto group = Snapshot.find(std::string(Request.LogicalName));
   if (group IS Snapshot.end()) {
      result.Error = ImportResolutionError::MissingGroup;
      return result;
   }

   for (auto candidate = group->second.rbegin(); candidate != group->second.rend(); ++candidate) {
      if (Request.Requirement) {
         Version version;
         if (parse_version(candidate->Version, version) or
             not satisfies(version, Request.Requirement->Constraint)) continue;
      }
      result.Import.ResolvedPath = "packages:" + candidate->RelativePath;
      result.Import.SelectedVersion = candidate->Version;
      result.Import.PackageManaged = true;
      return result;
   }

   result.Error = ImportResolutionError::UnsatisfiedConstraint;
   return result;
}

//********************************************************************************************************************
// Resolve the selected import to a file through the packages volume.

ImportResolutionResult resolve_package_import(const PackageResolverSnapshot &Snapshot,
   const ImportResolutionRequest &Request)
{
   auto result = select_package_import(Snapshot, Request);
   if (not result) return result;

   std::string resolved;
   if (ResolvePath(result.Import.ResolvedPath, RSF::NIL, &resolved) != ERR::Okay) {
      result.Error = ImportResolutionError::MissingTarget;
      result.Detail = result.Import.ResolvedPath;
      return result;
   }

   LOC type = LOC::NIL;
   if (AnalysePath(resolved, &type) != ERR::Okay or type != LOC::FILE) {
      result.Error = ImportResolutionError::MissingTarget;
      result.Detail = result.Import.ResolvedPath;
      return result;
   }

   // Source readers check readability when capturing the compilation snapshot.  Opening here duplicates that I/O
   // during cache validation and needlessly opens sources even when an active module satisfies the import.

   result.Import.ResolvedPath = std::move(resolved);
   return result;
}

//********************************************************************************************************************
// Resolve an import using the current global package index snapshot.

ImportResolutionResult resolve_package_import(const ImportResolutionRequest &Request)
{
   if (not glPackageResolver) {
      ImportResolutionResult result;
      result.Import.LogicalName.assign(Request.LogicalName);
      result.Error = ImportResolutionError::UnavailableIndex;
      return result;
   }
   return resolve_package_import(*glPackageResolver, Request);
}

//********************************************************************************************************************
// Turn a resolution error into a message that names the failed import and relevant target or constraint.

std::string import_resolution_error(const ImportResolutionRequest &Request, const ImportResolutionResult &Result)
{
   const std::string constraint = Request.Requirement ? Request.Requirement->Constraint.Original : std::string();
   switch (Result.Error) {
      case ImportResolutionError::Okay: return {};
      case ImportResolutionError::InvalidName:
         return std::format("Invalid package import name '{}'", Request.LogicalName);
      case ImportResolutionError::UnavailableIndex:
         return "The installed package index is unavailable";
      case ImportResolutionError::MissingGroup:
         return std::format("Package index has no public import named '{}'", Request.LogicalName);
      case ImportResolutionError::UnsatisfiedConstraint:
         return std::format("Package import '{}' has no version satisfying '{}'", Request.LogicalName, constraint);
      case ImportResolutionError::InvalidTarget:
         return std::format("Package import '{}' has an invalid indexed target", Request.LogicalName);
      case ImportResolutionError::MissingTarget:
         return std::format("Package import '{}' selected version {} at '{}', but the file is unavailable",
            Request.LogicalName, Result.Import.SelectedVersion.value_or("<unknown>"), Result.Detail);
   }
   return std::format("Package import '{}' could not be resolved", Request.LogicalName);
}

//********************************************************************************************************************
// Verify that an indexed import declares valid package metadata for the selected version.

bool check_resolved_package(const ResolvedImport &Import, const std::optional<PackageIdentity> &Declared,
   std::string &Diagnostic)
{
   if (not Import.PackageManaged) return true;

   if (not Declared) {
      Diagnostic = std::format("Package import '{}@{}' requires @Package metadata", Import.LogicalName,
         Import.SelectedVersion.value_or("<unknown>"));
      return false;
   }

   if (validate_package_identity(*Declared) != PackageValidationError::OKAY) {
      Diagnostic = std::format("Package import '{}@{}' loaded invalid @Package metadata", Import.LogicalName,
         Import.SelectedVersion.value_or("<unknown>"));
      return false;
   }

   if (not Import.SelectedVersion or Declared->Version != *Import.SelectedVersion) {
      Diagnostic = std::format("Package import '{}' selected version {}, but source declares package '{}@{}'",
         Import.LogicalName, Import.SelectedVersion.value_or("<unknown>"), Declared->Name, Declared->Version);
      return false;
   }
   return true;
}

} // namespace tiri
