/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

This version of the launcher is intended for use from the command-line only.

*********************************************************************************************************************/

#include <kotuku/main.h>
#include <kotuku/modules/core.h>
#include <kotuku/modules/display.h>
#include <kotuku/modules/script.h>
#include <kotuku/startup.h>
#include <kotuku/strings.hpp>
#include <string.h>

#include "common.h"
#include "version.h"

extern struct CoreBase *CoreBase;

static std::string glProcedure;
static objSurface *glTarget = nullptr;
std::span<std::string> glArgs;
static int glArgsIndex = 0;
//static STRING glAllow = nullptr;
static std::string glTargetFile;
static std::string glStatement;
static objTask *glTask = nullptr;
static objScript *glScript = nullptr;
static bool glSandbox = false;
static bool glRelaunched = false;
static bool glTime = false;
static bool glDialog = false;
static bool glBackstage = false;

static ERR exec_source(std::string_view, int, const std::string_view);

#ifdef _WIN32
inline void select_window_icon_resource(const std::string_view Path)
{
   bool is_tiri = (Path.size() >= 5) and kt::iequals(Path.substr(Path.size() - 5), ".tiri");
   SetResource(RES::WINDOWS_ICON, is_tiri ? 501 : 500);
}
#else
inline void select_window_icon_resource(const std::string_view) { }
#endif

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
 --log-file      Write log output to the given file path.
 --jit-options   Development options that control the behaviour of the compiler.
 --version       Prints the version number on line 1 and git commit on line 2.
)";

static std::string glDialogScript =
R"(string:import 'gui/filedialog'
try
 gui.dialog.file({
  filterList = { { name='Script Files', ext='.tiri' } },
  strictFilter = true,
  title = 'Run a Script',
  okText = 'Run Script',
  cancelText = 'Exit',
  path = '%%PATH%%',
  feedback = function(Dialog, Path, Files)
   if not Files then mSys.SendMessage(MSGID_QUIT) return end
   global glRunFile = Path .. Files[0].filename
   processing.signal()
  end
 })
except ex
 raise ERR_NoSupport
end
processing.sleep()
if glRunFile then
 obj.new('tiri', { src = glRunFile }).acActivate()
end
)";

//********************************************************************************************************************

#include "exec.cpp"

//********************************************************************************************************************

constexpr uint32_t ARG_HELP1       = kt::strhash("--help");
constexpr uint32_t ARG_HELP2       = kt::strhash("help");
constexpr uint32_t ARG_VERSION     = kt::strhash("--version");
constexpr uint32_t ARG_VERIFY      = kt::strhash("--verify");
constexpr uint32_t ARG_SANDBOX     = kt::strhash("--sandbox");
constexpr uint32_t ARG_TIME        = kt::strhash("--time");
constexpr uint32_t ARG_DIALOG      = kt::strhash("--dialog");
constexpr uint32_t ARG_RELAUNCH    = kt::strhash("--relaunch");
constexpr uint32_t ARG_BACKSTAGE   = kt::strhash("--backstage");
constexpr uint32_t ARG_PROCEDURE   = kt::strhash("--procedure");
constexpr uint32_t ARG_STATEMENT   = kt::strhash("--statement");
constexpr uint32_t ARG_JIT_OPTIONS = kt::strhash("--jit-options");
constexpr uint32_t ARG_LOG_FILE    = kt::strhash("--log-file");
constexpr uint32_t ARG_C           = kt::strhash("-c");
constexpr uint32_t ARG_E           = kt::strhash("-e");

static ERR process_args(void)
{
   kt::Log log("Origo");

   if ((!glTask->getParameters(glArgs))) {
      for (unsigned i=0; i < glArgs.size(); i++) {
         auto hash = kt::strhash(glArgs[i]);
         if (hash IS ARG_HELP1 or hash IS ARG_HELP2) { // Print help for the user
            printf("%s", glHelp.c_str());
            return ERR::Terminate;
         }
         else if (hash IS ARG_VERSION) { // Print version information
            printf("%s\n", KOTUKU_VERSION);
            printf("%s:%s\n", KOTUKU_GIT_BRANCH, KOTUKU_GIT_COMMIT);
            printf("Build Type: %s\n", KOTUKU_BUILD_TYPE);
            return ERR::Terminate;
         }
         else if (hash IS ARG_VERIFY) { // Dummy option for verifying installs
            return ERR::Terminate;
         }
         else if (hash IS ARG_SANDBOX) {
            glSandbox = true;
         }
         else if (hash IS ARG_TIME) {
            glTime = true;
         }
         else if (hash IS ARG_DIALOG) {
            // Display a file dialog for choosing a script manually
            glDialog = true;
         }
         else if (hash IS ARG_RELAUNCH) {
            // Internal argument to detect relaunching at an altered security level
            glRelaunched = true;
         }
         else if (hash IS ARG_BACKSTAGE) {
            glBackstage = true;
            if (i + 1 < glArgs.size()) {
               if (atoi(glArgs[i+1].c_str()) > 0) i++; // Port is optional
            }
         }
         else if (hash IS ARG_PROCEDURE) {
            if (i + 1 < glArgs.size()) {
               glProcedure.assign(glArgs[i+1]);
               i++;
            }
         }
         else if (hash IS ARG_STATEMENT or hash IS ARG_C or hash IS ARG_E) {
            // NB: The support for -c and -e exists only for AI agents that like to use this syntax for whatever reason...
            if (i + 1 < glArgs.size()) {
               glStatement.assign(glArgs[i+1]);
               i++;
            }
         }
         else if (hash IS ARG_JIT_OPTIONS or hash IS ARG_LOG_FILE) {
            // For some system parameters we need to skip the next argument.
            if (i + 1 < glArgs.size()) i++;
         }
         else if (kt::startswith("--", glArgs[i])) {
            // Unrecognised argument beginning with '--', ignore it.
         }
         else { // If argument not recognised, assume this arg is the target file.
            if (ResolvePath(glArgs[i], RSF::APPROXIMATE, &glTargetFile) != ERR::Okay) {
               printf("Unable to find file '%s'\n", glArgs[i].c_str());
               return ERR::Terminate;
            }
            glArgsIndex = i + 1;
            select_window_icon_resource(glTargetFile);
            break;
         }
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR select_file_dialog(void)
{
   auto driver = CSTRING(GetResourcePtr(RES::DISPLAY_DRIVER));
   if ((driver) and kt::iequals(driver, "headless")) return ERR::NoSupport;

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

   return exec_source(glDialogScript, glTime, glProcedure);
}

//********************************************************************************************************************
// Note: In Windows, if the program is failing to load and no output is printed, pipe to Out-Host to see error messages.
// E.g. .\origo.exe --version | Out-Host

extern "C" int main(int argc, char **argv)
{
   kt::Log log("Origo");

   if (auto msg = init_kotuku(argc, (CSTRING *)argv)) {
      for (int i=1; i < argc; i++) { // If in --verify mode, return with no error code and print nothing.
         if (not strcmp(argv[i], "--verify")) return 0;
      }
      printf("%s\n", msg);
      return -1;
   }

   glTask = CurrentTask();
   select_window_icon_resource("");

   ERR result = ERR::Okay;
   if (!process_args()) {
      if (glBackstage) {
         objModule::load("backstage");
      }

      if (glDialog) {
         select_window_icon_resource(".tiri");
         result = select_file_dialog();
      }
      else if (not glStatement.empty()) {
         result = exec_source(std::string("string:") + glStatement, glTime, glProcedure);
      }
      else if (not glTargetFile.empty()) {
         std::string_view path;
         if (!glTask->getPath(path)) log.msg("Path: %.*s", int(path.size()), path.empty() ? "" : path.data());
         else log.error("No working path.");

         LOC type;
         if ((AnalysePath(glTargetFile, &type) != ERR::Okay) or (type != LOC::FILE)) {
            printf("File '%s' does not exist.\n", glTargetFile.c_str());
         }
         else result = exec_source(glTargetFile, glTime, glProcedure);
      }
      else {
         // Engage default behaviour if no parameters have been specified
         // Check for the presence of package.zip or main.tiri files in the working directory

         std::string_view path;
         glTask->getProcessPath(path);
         if (path.empty()) path = ".";
         std::string exe_path(path);
         if (not ((exe_path.ends_with("/")) or (exe_path.ends_with("\\")))) {
            exe_path.append("/");
         }

         auto pkg_path = exe_path + "package.zip";

         LOC type;
         static objCompression *glPackageArchive;
         if ((!AnalysePath(pkg_path, &type)) and (type IS LOC::FILE)) {
            // Create a "package:" volume and attempt to run "package:main.tiri"
            if ((glPackageArchive = objCompression::create::local(fl::Path(pkg_path), fl::ArchiveName("package"), fl::Flags(CMF::READ_ONLY)))) {
               if (SetVolume("package", "archive:package/", "filetypes/archive", "", "", VOLUME::REPLACE|VOLUME::HIDDEN) != ERR::Okay) return -1;

               select_window_icon_resource("package:main.tiri");
               result = exec_source("package:main.tiri", glTime, glProcedure);
            }
            else return -1;
         }
         else { // Check for main.tiri
            if ((!AnalysePath("main.tiri", &type)) and (type IS LOC::FILE)) {
               select_window_icon_resource("main.tiri");
               result = exec_source("main.tiri", glTime, glProcedure);
            }
            else {
               select_window_icon_resource(".tiri");
               auto error = select_file_dialog();

               if (error IS ERR::NoSupport) {
                  printf("%s", glHelp.c_str());
                  error = ERR::Okay;
               }

               result = error;
            }
         }
      }
   }

   if (glScript) { FreeResource(glScript); glScript = nullptr; }

   close_kotuku();

   return int(result);
}
