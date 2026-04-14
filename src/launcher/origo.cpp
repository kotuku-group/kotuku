/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

This version of the launcher is intended for use from the command-line only.

*********************************************************************************************************************/

#include <kotuku/main.h>
#include <kotuku/modules/core.h>
#include <kotuku/modules/display.h>
#include <kotuku/startup.h>
#include <kotuku/strings.hpp>
#include <string.h>

#include "common.h"
#include "version.h"

extern struct CoreBase *CoreBase;

static std::string glProcedure;
static objSurface *glTarget = nullptr;
pf::vector<std::string> *glArgs;
static int glArgsIndex = 0;
//static STRING glAllow = nullptr;
static std::string glTargetFile;
static std::string glStatement;
static OBJECTPTR glTask = nullptr;
static objScript *glScript = nullptr;
static bool glSandbox = false;
static bool glRelaunched = false;
static bool glTime = false;
static bool glDialog = false;
static bool glBackstage = false;

static ERR exec_source(std::string, int, const std::string);

static const std::string glHelp =
   "Origo " KOTUKU_VERSION R"(

Origo launches Tiri scripts and PARC files developed for Kotuku.

   origo [options] [script.ext] arg1 arg2=value ...

The following options can be used when executing script files:

 --procedure [n] The name of a procedure to execute.
 --time          Print the amount of time that it took to execute the script.
 --dialog        Display a file dialog for choosing a script manually.
 --backstage     Enables the backstage REST API (see Wiki).
 --statement     Instead of running a script file, executes a single statement or expression.

 --log-api       Activates run-time log messages at API level.
 --log-info      Activates run-time log messages at INFO level.
 --log-error     Activates run-time log messages at ERROR level.
 --jit-options   Development options that control the behaviour of the compiler.
 --version       Prints the version number on line 1 and git commit on line 2.
)";

static std::string glDialogScript =
R"(STRING:import 'gui/filedialog'
gui.dialog.file({
 filterList = { { name='Script Files', ext='.tiri' } },
 title      = 'Run a Script',
 okText     = 'Run Script',
 cancelText = 'Exit',
 path       = '%%PATH%%',
 feedback = function(Dialog, Path, Files)
  if not Files then mSys.SendMessage(MSGID_QUIT) return end
  global glRunFile = Path .. Files[0].filename
  processing.signal()
 end
})
processing.sleep(nil, true)
if glRunFile then obj.new('script', { src = glRunFile }).acActivate() end
)";

//********************************************************************************************************************

#include "exec.cpp"

//********************************************************************************************************************

static ERR process_args(void)
{
   pf::Log log("Origo");

   if ((glTask->get(FID_Parameters, glArgs) IS ERR::Okay) and (glArgs)) {
      pf::vector<std::string> &args = *glArgs;
      for (unsigned i=0; i < args.size(); i++) {
         if (pf::iequals(args[i], "--help")) { // Print help for the user
            printf("%s", glHelp.c_str());
            return ERR::Terminate;
         }
         else if (pf::iequals(args[i], "--version")) { // Print version information
            printf("%s\n", KOTUKU_VERSION);
            printf("%s:%s\n", KOTUKU_GIT_BRANCH, KOTUKU_GIT_COMMIT);
            printf("Build Type: %s\n", KOTUKU_BUILD_TYPE);
            return ERR::Terminate;
         }
         else if (pf::iequals(args[i], "--verify")) { // Dummy option for verifying installs
            return ERR::Terminate;
         }
         else if (pf::iequals(args[i], "--sandbox")) {
            glSandbox = true;
         }
         else if (pf::iequals(args[i], "--time")) {
            glTime = true;
         }
         else if (pf::iequals(args[i], "--dialog")) {
            // Display a file dialog for choosing a script manually
            glDialog = true;
         }
         else if (pf::iequals(args[i], "--relaunch")) {
            // Internal argument to detect relaunching at an altered security level
            glRelaunched = true;
         }
         else if (pf::iequals(args[i], "--backstage")) {
            glBackstage = true;
            if (i + 1 < args.size()) i++;
         }
         else if (pf::iequals(args[i], "--procedure")) {
            if (i + 1 < args.size()) {
               glProcedure.assign(args[i+1]);
               i++;
            }
         }
         else if (pf::iequals(args[i], "--statement") or (pf::iequals(args[i], "-c")) or (pf::iequals(args[i], "-e"))) {
            // NB: The support for -c and -e exists only for AI agents that like to use this syntax for whatever reason...
            if (i + 1 < args.size()) {
               glStatement.assign(args[i+1]);
               i++;
            }
         }
         else if (pf::iequals(args[i], "--jit-options")) {
            // Handled by the Tiri module, we just need to skip the next argument.
            if (i + 1 < args.size()) i++;
         }
         else if (pf::startswith("--", args[i])) {
            // Unrecognised argument beginning with '--', ignore it.
         }
         else { // If argument not recognised, assume this arg is the target file.
            if (ResolvePath(args[i], RSF::APPROXIMATE, &glTargetFile) != ERR::Okay) {
               printf("Unable to find file '%s'\n", args[i].c_str());
               return ERR::Terminate;
            }
            glArgsIndex = i + 1;
            break;
         }
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Note: In Windows, if the program is failing to load and no output is printed, pipe to Out-Host to see error messages.
// E.g. .\origo.exe --version | Out-Host

extern "C" int main(int argc, char **argv)
{
   pf::Log log("Origo");

   if (auto msg = init_kotuku(argc, (CSTRING *)argv)) {
      for (int i=1; i < argc; i++) { // If in --verify mode, return with no error code and print nothing.
         if (not strcmp(argv[i], "--verify")) return 0;
      }
      printf("%s\n", msg);
      return -1;
   }

   glTask = CurrentTask();

   int result = 0;
   if (process_args() IS ERR::Okay) {
      if (glBackstage) {
         objModule::load("backstage");
      }

      if (glDialog) {
         auto start = glDialogScript.find("%%PATH%%");
         if (not glTargetFile.empty()) {
            std::string::size_type n = 0;
            while ((n = glTargetFile.find('\\', n)) != std::string::npos) {
                glTargetFile.replace(n, 1, "\\\\");
                n += 2;
            }

            glDialogScript.replace(start, 8, glTargetFile);
         }
         else glDialogScript.replace(start, 8, "kotuku:");

         result = int(exec_source(glDialogScript.c_str(), glTime, glProcedure));
      }
      else if (not glStatement.empty()) {
         result = int(exec_source(std::string("STRING:") + glStatement, glTime, glProcedure));
      }
      else if (not glTargetFile.empty()) {
         CSTRING path;
         if (glTask->get(FID_Path, path) IS ERR::Okay) log.msg("Path: %s", path);
         else log.error("No working path.");

         LOC type;
         if ((AnalysePath(glTargetFile.c_str(), &type) != ERR::Okay) or (type != LOC::FILE)) {
            printf("File '%s' does not exist.\n", glTargetFile.c_str());
         }
         else result = int(exec_source(glTargetFile.c_str(), glTime, glProcedure));
      }
      else {
         // Check for the presence of package.zip or main.tiri files in the working directory

         auto path = glTask->get<CSTRING>(FID_ProcessPath);
         if ((not path) or (not path[0])) path = ".";
         std::string exe_path(path);
         if (not ((exe_path.ends_with("/")) or (exe_path.ends_with("\\")))) {
            exe_path.append("/");
         }

         auto pkg_path = exe_path + "package.zip";

         LOC type;
         static objCompression *glPackageArchive;
         if ((AnalysePath(pkg_path.c_str(), &type) IS ERR::Okay) and (type IS LOC::FILE)) {
            // Create a "package:" volume and attempt to run "package:main.tiri"
            if ((glPackageArchive = objCompression::create::local(fl::Path(pkg_path), fl::ArchiveName("package"), fl::Flags(CMF::READ_ONLY)))) {
               if (SetVolume("package", "archive:package/", "filetypes/archive", nullptr, nullptr, VOLUME::REPLACE|VOLUME::HIDDEN) != ERR::Okay) return -1;

               result = (int)exec_source("package:main.tiri", glTime, glProcedure);
            }
            else return -1;
         }
         else { // Check for main.tiri
            if ((AnalysePath("main.tiri", &type) IS ERR::Okay) and (type IS LOC::FILE)) {
               result = (int)exec_source("main.tiri", glTime, glProcedure);
            }
            else printf("%s", glHelp.c_str());
         }
      }
   }

   if (glScript) { FreeResource(glScript); glScript = nullptr; }

   close_kotuku();

   return result;
}
