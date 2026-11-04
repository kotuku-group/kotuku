#include <kotuku/main.h>

#include "../packaging/package_resolver.h"

namespace {

bool expect(bool Condition, kt::Log &Log, const char *Message)
{
   if (Condition) return true;
   Log.error("%s", Message);
   return false;
}

bool package_resolver_tests(kt::Log &Log)
{
   ConfigGroups groups = {
      { "example/tool", {
         { "1.10", "example/1.10/tool.tiri" },
         { "1", "example/1/tool.tiri" },
         { "2", "example/2/tool.tiri" },
         { "1.9", "example/1.9/tool.tiri" },
         { "1.0", "example/1.0/tool.tiri" }
      } }
   };
   std::shared_ptr<const tiri::PackageResolverSnapshot> snapshot;
   std::string diagnostic;
   if (not expect(tiri::build_package_resolver_snapshot(groups, snapshot, diagnostic) IS
       tiri::ImportResolutionError::Okay, Log, "A valid package index snapshot was rejected")) return false;

   auto highest = tiri::select_package_import(*snapshot, { "example/tool", nullptr });
   if (not expect(highest and highest.Import.SelectedVersion IS "2", Log,
       "Unversioned selection did not choose the highest numeric version")) return false;

   tiri::VersionConstraint constraint;
   if (tiri::parse_version_constraint(">=1.0 <2", constraint)) return false;
   tiri::PackageImportRequirement requirement { "example/tool", constraint };
   auto ranged = tiri::select_package_import(*snapshot, { "example/tool", &requirement });
   if (not expect(ranged and ranged.Import.SelectedVersion IS "1.10", Log,
       "Ranged selection did not choose version 1.10")) return false;

   if (tiri::parse_version_constraint(">2", constraint)) return false;
   requirement.Constraint = constraint;
   auto unsatisfied = tiri::select_package_import(*snapshot, { "example/tool", &requirement });
   if (not expect(unsatisfied.Error IS tiri::ImportResolutionError::UnsatisfiedConstraint, Log,
       "An indexed unsatisfied constraint incorrectly used the legacy fallback")) return false;

   auto missing = tiri::select_package_import(*snapshot, { "legacy/tool", nullptr });
   if (not expect(missing.Error IS tiri::ImportResolutionError::MissingGroup, Log,
       "A missing public import unexpectedly resolved")) return false;
   auto invalid_name = tiri::select_package_import(*snapshot, { "Legacy/Tool", nullptr });
   if (not expect(invalid_name.Error IS tiri::ImportResolutionError::InvalidName, Log,
       "An invalid public import unexpectedly resolved")) return false;

   static constexpr std::string_view unsafe_paths[] = {
      "", "/absolute.tiri", "volume:file.tiri", "../escape.tiri", "a/./b.tiri", "a//b.tiri", "a\\b.tiri"
   };
   for (auto path : unsafe_paths) {
      if (not expect(not tiri::package_index_path_is_safe(path), Log, "An unsafe package path was accepted")) {
         return false;
      }
   }

   ConfigGroups invalid = { { "Invalid", { { "1", "valid/file.tiri" } } } };
   if (not expect(tiri::build_package_resolver_snapshot(invalid, snapshot, diagnostic) IS
       tiri::ImportResolutionError::InvalidName, Log, "An invalid public import name was accepted")) return false;
   invalid = { { "valid", { { "01", "valid/file.tiri" } } } };
   if (not expect(tiri::build_package_resolver_snapshot(invalid, snapshot, diagnostic) IS
       tiri::ImportResolutionError::InvalidTarget, Log, "A non-canonical package version was accepted")) return false;
   invalid = { { "valid", {} } };
   if (not expect(tiri::build_package_resolver_snapshot(invalid, snapshot, diagnostic) IS
       tiri::ImportResolutionError::UnavailableIndex, Log, "An empty package group was accepted")) return false;
   invalid.clear();
   return expect(tiri::build_package_resolver_snapshot(invalid, snapshot, diagnostic) IS
      tiri::ImportResolutionError::UnavailableIndex, Log, "An empty package index was accepted");
}

} // namespace

void package_resolver_unit_tests(int &Passed, int &Total)
{
   Total++;
   kt::Log log("PackageResolverTests");
   if (package_resolver_tests(log)) Passed++;
}
