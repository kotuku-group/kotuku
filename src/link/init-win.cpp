/*********************************************************************************************************************

This file is in the public domain and may be distributed and modified without restriction.

*********************************************************************************************************************/

#include <stdio.h>
#include <mutex>
#include <string>

#include <kotuku/main.h>

#ifndef KOTUKU_STATIC
#define DLLCALL // __declspec(dllimport)
#define WINAPI  __stdcall

extern "C" {
DLLCALL APTR WINAPI GetProcAddress(APTR, CSTRING);
DLLCALL int WINAPI FreeLibrary(APTR);
}
using OPENCORE = ERR(struct OpenInfo *, struct CoreBase **);
using CLOSECORE = void(void);

struct CoreBase *CoreBase;
static APTR find_core();
static APTR corehandle = nullptr;
CLOSECORE *CloseCore = nullptr;
#else
static struct CoreBase *CoreBase; // Dummy
#endif

static std::mutex glInitLock;

//********************************************************************************************************************

extern "C" const char * init_kotuku(int argc, CSTRING *argv)
{
   std::lock_guard<std::mutex> lock(glInitLock);

#ifndef KOTUKU_STATIC
   corehandle = find_core();
   if (!corehandle) return "Failed to open Kotuku's core library.";

   auto OpenCore = (OPENCORE *)GetProcAddress((APTR)corehandle, "OpenCore");
   if (!OpenCore) {
      FreeLibrary(corehandle);
      corehandle = nullptr;
      return "Could not find the OpenCore symbol in Kotuku.";
   }

   if (!(CloseCore = (CLOSECORE *)GetProcAddress((APTR)corehandle, "CloseCore"))) {
      FreeLibrary(corehandle);
      corehandle = nullptr;
      return "Could not find the CloseCore symbol in Kotuku.";
   }
#endif

   struct OpenInfo info;
   info.Options     = nullptr;
   info.Detail      = 0;
   info.MaxDepth    = 14;
   info.Args        = argv;
   info.ArgCount    = argc;
   info.Flags       = OPF::ARGS;

   if (auto error = OpenCore(&info, &CoreBase); !error) return nullptr;
   else {
#ifndef KOTUKU_STATIC
      FreeLibrary(corehandle);
      corehandle = nullptr;
      CloseCore  = nullptr;
#endif
      if (error IS ERR::CoreVersion) {
         return "This program requires the latest version of the Kotuku framework.\nPlease visit www.kotuku.dev to upgrade.";
      }

      static char msgbuf[120];
      snprintf(msgbuf, sizeof(msgbuf), "Failed to initialise Kotuku, error code %d.", int(error));
      return msgbuf;
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

   if (corehandle) {
      FreeLibrary(corehandle);
      corehandle = nullptr;
   }
#else
   CloseCore();
#endif
}

#include "common-win.cpp"
