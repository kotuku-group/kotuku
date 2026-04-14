/*********************************************************************************************************************

This file is in the public domain and may be distributed and modified without restriction.

*********************************************************************************************************************/

#include <stdio.h>
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

//********************************************************************************************************************

extern "C" const char * init_kotuku(int argc, CSTRING *argv)
{
#ifndef KOTUKU_STATIC
   corehandle = find_core();
   if (!corehandle) return "Failed to open Kotuku's core library.";

   auto OpenCore = (OPENCORE *)GetProcAddress((APTR)corehandle, "OpenCore");
   if (!OpenCore) {
      FreeLibrary(corehandle);
      return "Could not find the OpenCore symbol in Kotuku.";
   }

   if (!(CloseCore = (CLOSECORE *)GetProcAddress((APTR)corehandle, "CloseCore"))) {
      FreeLibrary(corehandle);
      return "Could not find the CloseCore symbol in Kotuku.";
   }
#endif

   struct OpenInfo info;
   info.Detail      = 0;
   info.MaxDepth    = 14;
   info.Args        = argv;
   info.ArgCount    = argc;
   info.Error = ERR::Okay;
   info.Flags = OPF::ARGS|OPF::ERROR;

   if (OpenCore(&info, &CoreBase) IS ERR::Okay) return nullptr;
   else if (info.Error IS ERR::CoreVersion) {
      return "This program requires the latest version of the Kotuku framework.\nPlease visit www.kotuku.dev to upgrade.";
   }
   else {
      static char msgbuf[120];
      snprintf(msgbuf, sizeof(msgbuf), "Failed to initialise Kotuku, error code %d.", int(info.Error));
      return msgbuf;
   }
}

//********************************************************************************************************************

extern "C" void close_kotuku(void)
{
   CloseCore();
#ifndef KOTUKU_STATIC
   FreeLibrary(corehandle);
#endif
}

#include "common-win.cpp"
