#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>

#include "../packaging/version_constraints.h"

#ifdef UNIT_TESTS
namespace {

bool version_and_constraint_tests(kt::Log &Log)
{
   using namespace tiri;
   Version one, one_zero, one_zero_one, one_one, one_ten, two;

   if (parse_version("1", one) or parse_version("1.0", one_zero) or
       parse_version("1.0.1", one_zero_one) or parse_version("1.1", one_one) or
       parse_version("1.10", one_ten) or parse_version("2", two)) return false;

   if (one.compare(one_zero) >= 0 or one_zero.compare(one_zero_one) >= 0 or
       one_zero_one.compare(one_one) >= 0 or one_one.compare(one_ten) >= 0 or
       one_ten.compare(two) >= 0) {
      Log.error("Version component ordering is incorrect");
      return false;
   }

   VersionConstraint constraint;
   if (parse_version_constraint(">=1.0 <2", constraint) or not satisfies(one_ten, constraint) or
       satisfies(two, constraint)) {
      Log.error("Bounded constraint evaluation is incorrect");
      return false;
   }

   for (std::string_view invalid : { "", "=1", "==1", "!=1", "> 1", "1,2", "1 and 2", "01", "1." }) {
      if (not parse_version_constraint(invalid, constraint)) {
         Log.error("Malformed constraint was accepted: %.*s", int(invalid.size()), invalid.data());
         return false;
      }
   }
   return true;
}

bool manifest_tests(kt::Log &Log)
{
   using namespace tiri;
   VersionConstraint tiri_constraint;
   VersionConstraint kotuku_constraint;

   if (parse_version_constraint(">=1.0 <2.0", tiri_constraint) or
       parse_version_constraint(">=2026.2.23", kotuku_constraint)) return false;

   std::vector<CompatibilityRecord> records {
      { "packages:z.tiri", { tiri_constraint, std::nullopt } },
      { "packages:a.tiri", { std::nullopt, kotuku_constraint } },
      { "packages:z.tiri", { tiri_constraint, std::nullopt } }
   };

   std::string encoded;
   if (encode_compatibility_manifest(records, encoded)) return false;
   std::vector<CompatibilityRecord> decoded;
   if (decode_compatibility_manifest(encoded, decoded) or decoded.size() != 2 or
       decoded[0].Owner != "packages:a.tiri" or decoded[1].Owner != "packages:z.tiri") {
      Log.error("Compatibility manifest did not canonicalise and round-trip");
      return false;
   }

   auto malformed = encoded;
   malformed.push_back('\0');
   if (not decode_compatibility_manifest(malformed, decoded)) {
      Log.error("Compatibility manifest trailing data was accepted");
      return false;
   }

   return true;
}

bool injected_runtime_tests(kt::Log &Log)
{
   using namespace tiri;
   RuntimeVersions runtime;

   if (parse_version("1.0", runtime.Tiri) or parse_version("2026.2.23", runtime.Kotuku)) return false;
   VersionConstraint future;

   if (parse_version_constraint(">=2.0", future)) return false;
   CompatibilityFailure failure;

   if (check_requirements({ future, std::nullopt }, runtime, &failure) or failure.Domain != "tiri" or
       not failure.Comparison or not failure.Running) {
      Log.error("Injected runtime did not report the first failed comparison");
      return false;
   }

   VersionConstraint compatible_language;
   RuntimeVersions later_runtime;
   if (parse_version_constraint("1.0", compatible_language) or
       parse_version("1.1", later_runtime.Tiri) or parse_version("2026.2.23", later_runtime.Kotuku) or
       not check_requirements({ compatible_language, std::nullopt }, later_runtime)) {
      Log.error("A later same-major Tiri runtime did not satisfy an unprefixed language requirement");
      return false;
   }

   RuntimeVersions next_major_runtime;
   if (parse_version("2.0", next_major_runtime.Tiri) or
       parse_version("2026.2.23", next_major_runtime.Kotuku) or
       check_requirements({ compatible_language, std::nullopt }, next_major_runtime)) {
      Log.error("A different-major Tiri runtime satisfied an unprefixed language requirement");
      return false;
   }

   VersionConstraint later_language;
   if (parse_version_constraint("1.1", later_language) or
       check_requirements({ later_language, std::nullopt }, runtime)) {
      Log.error("An older Tiri runtime satisfied a later unprefixed language requirement");
      return false;
   }

   VersionConstraint exact_kotuku;
   RuntimeVersions later_kotuku;
   if (parse_version_constraint("2026.2.23", exact_kotuku) or
       parse_version("1.0", later_kotuku.Tiri) or parse_version("2026.2.24", later_kotuku.Kotuku) or
       check_requirements({ std::nullopt, exact_kotuku }, later_kotuku)) {
      Log.error("The Tiri compatibility rule was incorrectly applied to an unprefixed Kotuku requirement");
      return false;
   }

   return true;
}

bool package_import_tests(kt::Log &Log)
{
   using namespace tiri;
   VersionConstraint constraint;
   if (parse_version_constraint(">=1.0 <2", constraint)) return false;
   PackageImportRequirement requirement { "example/pkg", constraint };
   PackageImportFailure failure;

   for (std::string_view version : { "1.0", "1.0.1", "1.1", "1.10" }) {
      if (not check_package_import(requirement, PackageIdentity { "example/pkg", std::string(version) }, &failure)) {
         Log.error("Compatible package version was rejected: %.*s", int(version.size()), version.data());
         return false;
      }
   }

   if (check_package_import(requirement, std::nullopt, &failure) or
       failure.Kind != PackageImportFailureKind::MissingMetadata) {
      Log.error("Missing package metadata was not reported");
      return false;
   }

   if (not check_package_import(requirement, PackageIdentity { "other/pkg", "1.1" }, &failure)) {
      Log.error("An exported import name was incorrectly required to match its owning package name");
      return false;
   }

   if (check_package_import(requirement, PackageIdentity { "Invalid", "1.1" }, &failure) or
       failure.Kind != PackageImportFailureKind::InvalidMetadata) {
      Log.error("An invalid declared package name was not reported");
      return false;
   }

   if (check_package_import(requirement, PackageIdentity { "example/pkg", "2" }, &failure) or
       failure.Kind != PackageImportFailureKind::Unsatisfied or not failure.Comparison or
       comparison_text(*failure.Comparison) != "<2") {
      Log.error("The first failed package comparison was not reported");
      return false;
   }

   for (std::string_view text : { "<1.1", "<=1.1", ">1.1", ">=1.1", "1.1" }) {
      VersionConstraint single;
      if (parse_version_constraint(text, single)) return false;
      PackageImportRequirement single_requirement { "example/pkg", std::move(single) };
      const bool expected = text IS "<=1.1" or text IS ">=1.1" or text IS "1.1";
      if (check_package_import(single_requirement, PackageIdentity { "example/pkg", "1.1" }) != expected) {
         Log.error("Package operator evaluation is incorrect for %.*s", int(text.size()), text.data());
         return false;
      }
   }

   return true;
}

} // namespace

void version_constraint_unit_tests(int &Passed, int &Total)
{
   kt::Log log("VersionConstraintTests");
   for (auto test : { version_and_constraint_tests, manifest_tests, injected_runtime_tests, package_import_tests }) {
      Total++;
      if (test(log)) Passed++;
   }
}
#endif
