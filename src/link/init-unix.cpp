/*********************************************************************************************************************

This file is in the public domain and may be distributed and modified without restriction.

*********************************************************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>

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
#else
static struct CoreBase *CoreBase; // Dummy
#endif

//********************************************************************************************************************

extern "C" const char * init_kotuku(int argc, CSTRING *argv)
{
   struct OpenInfo info = { .Flags = OPF::NIL };

#ifndef KOTUKU_STATIC
   char root_path[232] = ""; // NB: Assigned to info.RootPath
   char core_path[256] = "";

   // Check for a local installation in the CWD.

   struct stat corestat = { .st_size = -1 };
   if (!stat("lib/core.so", &corestat)) {
      // The working directory will form the root path to the Kotuku Framework
      if (getcwd(root_path, sizeof(root_path))) {
         int i = strlen(root_path);
         if (root_path[i-1] != '/') root_path[i++] = '/';
         root_path[i] = 0;
      }
      snprintf(core_path, sizeof(core_path), "%slib/core.so", root_path);
   }
   else {
      // Determine if there is a valid 'lib' folder in the binary's folder.
      // Retrieving the path of the binary only works on Linux (most types of Unix don't provide any support for this).

      char procfile[48];
      snprintf(procfile, sizeof(procfile), "/proc/%d/exe", getpid());

      int path_len;
      if ((path_len = readlink(procfile, root_path, sizeof(root_path)-1)) > 0) {
         // Strip the process name
         while ((path_len > 0) and (root_path[path_len-1] != '/')) path_len--;
         root_path[path_len] = 0;

         snprintf(core_path, sizeof(core_path), "%slib/core.so", root_path);
         if (stat(core_path, &corestat)) {
            // Check the parent folder of the binary
            path_len--;
            while ((path_len > 0) and (root_path[path_len-1] != '/')) path_len--;
            root_path[path_len] = 0;

            snprintf(core_path, sizeof(core_path), "%slib/core.so", root_path);
            if (stat(core_path, &corestat)) { // Support for fixed installations
               strncpy(root_path, _ROOT_PATH"/", sizeof(root_path));
               strncpy(core_path, _ROOT_PATH"/lib/kotuku/core.so", sizeof(core_path));
               if (stat(core_path, &corestat)) {
                  return "Failed to find the location of the core.so library";
               }
            }
         }
      }
   }

   if ((!core_path[0]) or (!(glCoreHandle = dlopen(core_path, RTLD_NOW)))) {
      fprintf(stderr, "%s: %s\n", core_path, dlerror());
      return "Failed to open the core library.";
   }

   auto OpenCore = (OPENCORE *)dlsym(glCoreHandle, "OpenCore");
   if (!OpenCore) return "Could not find the OpenCore symbol in the Core library.";

   CloseCore = (CLOSECORE *)dlsym(glCoreHandle, "CloseCore");
   if (!CloseCore) return "Could not find the CloseCore symbol.";

   info.RootPath  = root_path;
   info.Flags = OPF::ROOT_PATH;
#endif

   info.Detail    = 0;
   info.MaxDepth  = 14;
   info.Args      = argv;
   info.ArgCount  = argc;
   info.Error     = ERR::Okay;
   info.Flags     = OPF::ARGS|OPF::ERROR;

   if (OpenCore(&info, &CoreBase) IS ERR::Okay) return nullptr;
   else if (info.Error IS ERR::CoreVersion) return "This program requires the latest version of Kotuku.\nPlease visit www.kotuku.dev to upgrade.";
   else return "Failed to initialise Kotuku.  Run again with --log-info.";
}

//********************************************************************************************************************

extern "C" void close_kotuku(void)
{
   CloseCore();
#ifndef KOTUKU_STATIC
   if (glCoreHandle) dlclose(glCoreHandle);
#endif
}
