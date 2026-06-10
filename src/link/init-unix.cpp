/*********************************************************************************************************************

This file is in the public domain and may be distributed and modified without restriction.

*********************************************************************************************************************/

#include <stdio.h>
#include <array>
#include <dlfcn.h>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <sys/stat.h>

#include <kotuku/main.h>

#ifndef KOTUKU_STATIC

#ifndef _ROOT_PATH
#define _ROOT_PATH "/usr/local"
#endif

struct CoreBase *CoreBase;
static APTR glCoreHandle = nullptr;
using OPENCORE = ERR(struct OpenInfo *, struct CoreBase **);
using CLOSECORE = void(void);
static CLOSECORE *CloseCore = nullptr;

namespace {
constexpr std::string_view glCoreLibrary = "lib/core.so";
constexpr std::string_view glInstalledCoreLibrary = "lib/kotuku/core.so";
constexpr std::string_view glDefaultRootPath = _ROOT_PATH "/";
constexpr size_t glPathBufferSize = 4096;

static bool path_exists(const std::string &Path)
{
   struct stat file_info = { .st_size = -1 };
   return !stat(Path.c_str(), &file_info);
}

static bool path_exists(std::string_view Path)
{
   return path_exists(std::string(Path));
}

static void ensure_directory_path(std::string &Path)
{
   if ((!Path.empty()) and (!Path.ends_with('/'))) Path.push_back('/');
}

static bool trim_to_parent_directory(std::string &Path)
{
   while ((!Path.empty()) and (Path.ends_with('/'))) Path.pop_back();

   if (auto slash = Path.find_last_of('/'); slash != std::string::npos) {
      Path.resize(slash + 1);
      return true;
   }

   return false;
}

static std::string build_root_relative_path(std::string_view RootPath, std::string_view RelativePath)
{
   std::string path(RootPath);
   ensure_directory_path(path);
   path.append(RelativePath);
   return path;
}

static std::optional<std::string> get_working_directory()
{
   std::array<char, glPathBufferSize> buffer = {};

   if (!getcwd(buffer.data(), buffer.size())) return std::nullopt;

   std::string path(buffer.data());
   ensure_directory_path(path);
   return path;
}

static std::optional<std::string> get_process_directory()
{
   std::array<char, glPathBufferSize> buffer = {};
   std::string proc_file = "/proc/" + std::to_string(getpid()) + "/exe";

   if (auto path_len = readlink(proc_file.c_str(), buffer.data(), buffer.size() - 1); path_len > 0) {
      std::string path(buffer.data(), path_len);
      if (auto slash = path.find_last_of('/'); slash != std::string::npos) {
         path.resize(slash + 1);
         return path;
      }
   }

   return std::nullopt;
}

static bool resolve_core_location(std::string &RootPath, std::string &CorePath)
{
   if (path_exists(glCoreLibrary)) {
      if (auto path = get_working_directory()) {
         RootPath = *path;
         CorePath = build_root_relative_path(RootPath, glCoreLibrary);
         return true;
      }
   }

   // Determine if there is a valid 'lib' folder in the binary's folder.
   // Retrieving the path of the binary only works on Linux (most types of Unix don't provide any support for this).
   if (auto path = get_process_directory()) {
      RootPath = *path;
      CorePath = build_root_relative_path(RootPath, glCoreLibrary);
      if (path_exists(CorePath)) return true;

      if (trim_to_parent_directory(RootPath)) {
         CorePath = build_root_relative_path(RootPath, glCoreLibrary);
         if (path_exists(CorePath)) return true;
      }
   }

   RootPath = std::string(glDefaultRootPath);
   CorePath = build_root_relative_path(RootPath, glInstalledCoreLibrary);
   return path_exists(CorePath);
}
} // namespace
#else
static struct CoreBase *CoreBase; // Dummy
#endif

static std::mutex glInitLock;

//********************************************************************************************************************

extern "C" const char * init_kotuku(int argc, CSTRING *argv)
{
   std::lock_guard<std::mutex> lock(glInitLock);
   struct OpenInfo info = { .Flags = OPF::NIL };

#ifndef KOTUKU_STATIC
   std::string root_path;
   std::string core_path;

   if (!resolve_core_location(root_path, core_path)) {
      return "Failed to find the location of the core.so library";
   }

   if (!(glCoreHandle = dlopen(core_path.c_str(), RTLD_NOW))) {
      fprintf(stderr, "%s: %s\n", core_path.c_str(), dlerror());
      return "Failed to open the core library.";
   }

   auto OpenCore = (OPENCORE *)dlsym(glCoreHandle, "OpenCore");
   if (!OpenCore) {
      dlclose(glCoreHandle);
      glCoreHandle = nullptr;
      return "Could not find the OpenCore symbol in the Core library.";
   }

   CloseCore = (CLOSECORE *)dlsym(glCoreHandle, "CloseCore");
   if (!CloseCore) {
      dlclose(glCoreHandle);
      glCoreHandle = nullptr;
      return "Could not find the CloseCore symbol.";
   }

   info.RootPath  = root_path;
   info.Flags    |= OPF::ROOT_PATH;
#endif

   info.Detail    = 0;
   info.MaxDepth  = 14;
   info.Args      = argv;
   info.ArgCount  = argc;
   info.Flags    |= OPF::ARGS;

   if (auto error = OpenCore(&info, &CoreBase); !error) return nullptr;
   else {
#ifndef KOTUKU_STATIC
      dlclose(glCoreHandle);
      glCoreHandle = nullptr;
      CloseCore    = nullptr;
#endif
      if (error IS ERR::CoreVersion) return "This program requires the latest version of Kotuku.\nPlease visit www.kotuku.dev to upgrade.";
      else return "Failed to initialise Kotuku.  Run again with --log-api.";
   }
}

//********************************************************************************************************************

extern "C" void close_kotuku(void)
{
   std::lock_guard<std::mutex> lock(glInitLock);

#ifndef KOTUKU_STATIC
   if (CloseCore) {
      CloseCore();
      CloseCore = nullptr;
   }

   if (glCoreHandle) {
      dlclose(glCoreHandle);
      glCoreHandle = nullptr;
   }
#else
   CloseCore();
#endif
}
