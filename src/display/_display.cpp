/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

This is a dummy module for managing access to the display module.  It exists so that any program needing access to
the display API will be diverted to the module binary that is relevant to the platform (X11, DirectFB, OpenGL etc).

*********************************************************************************************************************/

#define __system__
#define PRV_DISPLAY
#define PRV_DISPLAY_MODULE

#ifdef __linux__
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#endif

#include <kotuku/modules/display.h>
#include "idl.h"

//********************************************************************************************************************

struct CoreBase *CoreBase;
static objModule *modDriver = nullptr;

//********************************************************************************************************************
// Module function list.

static int scrUnsupported(void)
{
   pf::Log log("Display");
   log.warning("Unhandled display function called - driver is not complete.");
   return 0;
}

#define FDEF static const FunctionField

FDEF argsAccessPointer[] = { { "Object", FD_OBJECTPTR }, { 0, 0 } };
FDEF argsCopyArea[] = { { "Error", FD_ERROR }, { "Bitmap", FD_OBJECTPTR }, { "Dest", FD_OBJECTPTR }, { "Flags", FD_INT }, { "X", FD_INT }, { "Y", FD_INT }, { "Width", FD_INT }, { "Height", FD_INT }, { "XDest", FD_INT }, { "YDest", FD_INT }, { 0, 0 } };
FDEF argsCopySurface[] = { { "Error", FD_ERROR }, { "BitmapSurface:Surface", FD_PTR|FD_STRUCT }, { "Bitmap", FD_OBJECTPTR }, { "Flags", FD_INT }, { "X", FD_INT }, { "Y", FD_INT }, { "Width", FD_INT }, { "Height", FD_INT }, { "XDest", FD_INT }, { "YDest", FD_INT }, { 0, 0 } };
FDEF argsDrawPixel[] = { { "Void", FD_VOID }, { "Bitmap", FD_OBJECTPTR }, { "X", FD_INT }, { "Y", FD_INT }, { "Colour", FD_INT }, { 0, 0 } };
FDEF argsDrawRGBPixel[] = { { "Void", FD_VOID }, { "Bitmap", FD_OBJECTPTR }, { "X", FD_INT }, { "Y", FD_INT }, { "RGB:RGB", FD_PTR|FD_STRUCT }, { 0, 0 } };
FDEF argsDrawRectangle[] = { { "Void", FD_VOID }, { "Bitmap", FD_OBJECTPTR }, { "X", FD_INT }, { "Y", FD_INT }, { "Width", FD_INT }, { "Height", FD_INT }, { "Colour", FD_INT }, { "Flags", FD_INT }, { 0, 0 } };
FDEF argsFlipBitmap[] = { { "Void", FD_VOID }, { "Bitmap", FD_OBJECTPTR }, { "Orientation", FD_INT }, { 0, 0 } };
FDEF argsGetColourFormat[] = { { "Void", FD_VOID }, { "ColourFormat:Format", FD_PTR|FD_STRUCT }, { "BitsPerPixel", FD_INT }, { "RedMask", FD_INT }, { "GreenMask", FD_INT }, { "BlueMask", FD_INT }, { "AlphaMask", FD_INT }, { 0, 0 } };
FDEF argsGetCursorInfo[] = { { "Error", FD_ERROR }, { "CursorInfo:Info", FD_PTR|FD_STRUCT }, { "Size", FD_INT|FD_BUFSIZE }, { 0, 0 } };
FDEF argsGetCursorPos[] = { { "Error", FD_ERROR }, { "X", FD_INT|FD_RESULT }, { "Y", FD_INT|FD_RESULT }, { 0, 0 } };
FDEF argsGetDisplayInfo[] = { { "Error", FD_ERROR }, { "Display", FD_OBJECTID }, { "DisplayInfo:Info", FD_PTR|FD_STRUCT }, { "Size", FD_INT|FD_BUFSIZE }, { 0, 0 } };
FDEF argsGetDisplayType[] = { { "Result", FD_INT }, { 0, 0 } };
FDEF argsGetInputEvent[] = { { "Error", FD_ERROR }, { "dcInputReady:Input", FD_PTR|FD_STRUCT }, { "Flags", FD_INT }, { "InputEvent:Msg", FD_PTR|FD_STRUCT|FD_RESULT }, { 0, 0 } };
FDEF argsGetInputTypeName[] = { { "Result", FD_STR }, { "Type", FD_INT }, { 0, 0 } };
FDEF argsGetRelativeCursorPos[] = { { "Error", FD_ERROR }, { "Surface", FD_OBJECTID }, { "X", FD_INT|FD_RESULT }, { "Y", FD_INT|FD_RESULT }, { 0, 0 } };
FDEF argsLockCursor[] = { { "Error", FD_ERROR }, { "Surface", FD_OBJECTID }, { 0, 0 } };
FDEF argsReadPixel[] = { { "Result", FD_INT }, { "Bitmap", FD_OBJECTPTR }, { "X", FD_INT }, { "Y", FD_INT }, { 0, 0 } };
FDEF argsReadRGBPixel[] = { { "Void", FD_VOID }, { "Bitmap", FD_OBJECTPTR }, { "X", FD_INT }, { "Y", FD_INT }, { "RGB:RGB", FD_PTR|FD_STRUCT }, { 0, 0 } };
FDEF argsResample[] = { { "Error", FD_ERROR }, { "Bitmap", FD_OBJECTPTR }, { "ColourFormat:ColourFormat", FD_PTR|FD_STRUCT }, { 0, 0 } };
FDEF argsRestoreCursor[] = { { "Error", FD_ERROR }, { "Cursor", FD_INT }, { "Owner", FD_OBJECTID }, { 0, 0 } };
FDEF argsScaleToDPI[] = { { "Result", FD_DOUBLE }, { "Value", FD_DOUBLE }, { 0, 0 } };
FDEF argsScanDisplayModes[] = { { "Error", FD_ERROR }, { "Filter", FD_STR }, { "DisplayInfo:Info", FD_PTR|FD_STRUCT }, { "Size", FD_INT|FD_BUFSIZE }, { 0, 0 } };
FDEF argsSetClipRegion[] = { { "Void", FD_VOID }, { "Bitmap", FD_OBJECTPTR }, { "Number", FD_INT }, { "Left", FD_INT }, { "Top", FD_INT }, { "Right", FD_INT }, { "Bottom", FD_INT }, { "Terminate", FD_INT }, { 0, 0 } };
FDEF argsSetCursor[] = { { "Error", FD_ERROR }, { "Surface", FD_OBJECTID }, { "Flags", FD_INT }, { "Cursor", FD_INT }, { "Name", FD_STR }, { "Owner", FD_OBJECTID }, { 0, 0 } };
FDEF argsSetCursorPos[] = { { "Error", FD_ERROR }, { "X", FD_INT }, { "Y", FD_INT }, { 0, 0 } };
FDEF argsSetCustomCursor[] = { { "Error", FD_ERROR }, { "Surface", FD_OBJECTID }, { "Flags", FD_INT }, { "Bitmap", FD_OBJECTPTR }, { "HotX", FD_INT }, { "HotY", FD_INT }, { "Owner", FD_OBJECTID }, { 0, 0 } };
FDEF argsSetHostOption[] = { { "Error", FD_ERROR }, { "Option", FD_INT }, { "Value", FD_INT64 }, { 0, 0 } };
FDEF argsStartCursorDrag[] = { { "Error", FD_ERROR }, { "Source", FD_OBJECTID }, { "Item", FD_INT }, { "Datatypes", FD_STR }, { "Surface", FD_OBJECTID }, { 0, 0 } };
FDEF argsSubscribeInput[] = { { "Error", FD_ERROR }, { "Surface", FD_OBJECTID }, { "Mask", FD_INT }, { "Device", FD_OBJECTID }, { 0, 0 } };
FDEF argsSync[] = { { "Void", FD_VOID }, { "Bitmap", FD_OBJECTPTR }, { 0, 0 } };
FDEF argsUnlockCursor[] = { { "Error", FD_ERROR }, { "Surface", FD_OBJECTID }, { 0, 0 } };
FDEF argsUnsubscribeInput[] = { { "Void", FD_VOID }, { "Surface", FD_OBJECTID }, { 0, 0 } };

Function JumpTable[] = {
   { scrUnsupported, "GetDisplayInfo", argsGetDisplayInfo },
   { scrUnsupported, "GetDisplayType", argsGetDisplayType },
   { scrUnsupported, "SetCursor", argsSetCursor },
   { scrUnsupported, "RestoreCursor", argsRestoreCursor },
   { scrUnsupported, "GetCursorPos", argsGetCursorPos },
   { scrUnsupported, "SetCursorPos", argsSetCursorPos },
   { scrUnsupported, "GetRelativeCursorPos", argsGetRelativeCursorPos },
   { scrUnsupported, "GetCursorInfo", argsGetCursorInfo },
   { scrUnsupported, "SetCustomCursor", argsSetCustomCursor },
   { scrUnsupported, "AccessPointer", argsAccessPointer },
   { scrUnsupported, "ScanDisplayModes", argsScanDisplayModes },
   { scrUnsupported, "LockCursor", argsLockCursor },
   { scrUnsupported, "UnlockCursor", argsUnlockCursor },
   { scrUnsupported, "SetHostOption", argsSetHostOption },
   { scrUnsupported, "StartCursorDrag", argsStartCursorDrag },
   { scrUnsupported, "CopySurface", argsCopySurface },
   { scrUnsupported, "Sync", argsSync },
   { scrUnsupported, "Resample", argsResample },
   { scrUnsupported, "GetColourFormat", argsGetColourFormat },
   { scrUnsupported, "CopyArea", argsCopyArea },
   { scrUnsupported, "ReadRGBPixel", argsReadRGBPixel },
   { scrUnsupported, "ReadPixel", argsReadPixel },
   { scrUnsupported, "DrawRGBPixel", argsDrawRGBPixel },
   { scrUnsupported, "DrawPixel", argsDrawPixel },
   { scrUnsupported, "DrawRectangle", argsDrawRectangle },
   { scrUnsupported, "FlipBitmap", argsFlipBitmap },
   { scrUnsupported, "SetClipRegion", argsSetClipRegion },
   { scrUnsupported, "SubscribeInput", argsSubscribeInput },
   { scrUnsupported, "UnsubscribeInput", argsUnsubscribeInput },
   { scrUnsupported, "GetInputEvent", argsGetInputEvent },
   { scrUnsupported, "GetInputTypeName", argsGetInputTypeName },
   { scrUnsupported, "ScaleToDPI", argsScaleToDPI },
   { nullptr, nullptr, nullptr }
};

//********************************************************************************************************************

#if defined(__linux__) && !defined(__ANDROID__)
static int test_x11(STRING Path)
{
   pf::Log log("test_x11_socket");
   struct sockaddr_un sockname;
   int namelen, fd, err;
   int16_t i;

   if ((fd = socket(PF_UNIX, SOCK_STREAM, 0)) != -1) {
      fcntl(fd, F_SETFL, O_NONBLOCK);

      sockname.sun_family = AF_UNIX;
      clearmem(sockname.sun_path, sizeof(sockname.sun_path));
      for (i=0; i < sizeof(sockname.sun_path) AND (Path[i]); i++) sockname.sun_path[i] = Path[i];
      namelen = i + sizeof(sockname.sun_family);

      if (connect(fd, (struct sockaddr *)&sockname, namelen) < 0) {
         log.msg("Socket %s failed: %s", Path, strerror(errno));
         err = errno;
         close(fd);

         // If the error was ENOENT, the server may be starting up and we should try again.
         // If the error was EWOULDBLOCK or EINPROGRESS then the socket was non-blocking and we should poll using select
         // If the error was EINTR, the connect was interrupted and we should try again.

         if ((err IS EINTR)) return ERR_Okay;
         else if ((err IS EWOULDBLOCK) or (err IS EINPROGRESS)) return ERR_Okay;
         else {
            log.msg("Connect Error: %s", strerror(err));
            return ERR_Failed;
         }
      }
      else log.msg("Connected to %s", Path);

      close(fd);

      return ERR_Okay;
   }
   else log.msg("Failed to open a socket.");

   return ERR_Failed;
}
#endif

//********************************************************************************************************************

static ERROR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   int i;
   uint8_t display;
   CSTRING displaymod;
   enum {
      DISPLAY_AUTO=0,
      DISPLAY_X11,
      DISPLAY_NATIVE,
      DISPLAY_GLES1,
      DISPLAY_GLES2,
      DISPLAY_GLES3,
      DISPLAY_HEADLESS
   };

   CoreBase = argCoreBase;

   // Determine what module to load

   CSTRING driver_name;
   if ((driver_name = GetResourcePtr(RES::DISPLAY_DRIVER))) {
      if (iequals(driver_name, "native"))     display = DISPLAY_NATIVE;
      else if (iequals(driver_name, "vesa"))  display = DISPLAY_NATIVE;
      else if (iequals(driver_name, "vga"))   display = DISPLAY_NATIVE;
      else if (iequals(driver_name, "x11"))   display = DISPLAY_X11;
      else if (iequals(driver_name, "gles"))  display = DISPLAY_GLES1;
      else if (iequals(driver_name, "gles1")) display = DISPLAY_GLES1;
      else if (iequals(driver_name, "gles2")) display = DISPLAY_GLES2;
      else if (iequals(driver_name, "gles3")) display = DISPLAY_GLES3;
      else if (iequals(driver_name, "auto"))  display = DISPLAY_AUTO;
      else if (iequals(driver_name, "none"))  display = DISPLAY_HEADLESS;
      else if (iequals(driver_name, "none"))  display = DISPLAY_HEADLESS;
      else display = DISPLAY_AUTO;
   }
   else display = DISPLAY_AUTO;

   if (display IS DISPLAY_AUTO) {
#if defined(__linux__) && !defined(__ANDROID__)
      // Check if X11 is running by scanning /tmp/.X11-unix

      int16_t j;
      char buffer[] = "/tmp/.X11-unix/X10";
      STRING x11[] = { "X", "X0", "X1", "X2", "X3", "X5", "X6", "X7", "X8", "X9", "X10" };

      for (int16_t i=0; (i < std::ssize(x11)); i++) {
         for (j=0; x11[i][j]; j++) buffer[15+j] = x11[i][j];
         buffer[15+j] = 0;
         if (test_x11(buffer) IS ERR_Okay) {
            log.msg("X11 server detected in /tmp");
            display = DISPLAY_X11;
            break;
         }
      }

      // Check if X11 is running according to the process list
#endif
   }

   if (display IS DISPLAY_AUTO) display = DISPLAY_NATIVE;

   #ifdef _WIN32
      displaymod = "display-windows";
   #elif __ANDROID__
      if (display IS DISPLAY_GLES1) displaymod = "display-gles1";
      else if (display IS DISPLAY_GLES2) displaymod = "display-gles2";
      else if (display IS DISPLAY_GLES3) displaymod = "display-gles3";
      else displaymod = "display-gles1";
   #else
      displaymod = "display-x11";
   #endif

   log.msg("Using display driver '%s'", displaymod);

   APTR driver_base;
   if (objModule::load(displaymod, (OBJECTPTR *)&modDriver, &driver_base) != ERR_Okay) {
#if defined(__linux__) && !defined(__ANDROID__)
      if (display IS DISPLAY_X11) {
         static uint8_t x11_fail = FALSE;
         if (!x11_fail) {
            printf("An X Server needs to be running (try running 'kotuku-xserver' to automatically create one).\n");
            x11_fail = TRUE;
         }
         return ERR_InitModule;
      }
      else if (display IS DISPLAY_HEADLESS); // Do nothing
      else return ERR_InitModule;
#else
      return ERR_InitModule;
#endif
   }

   // All function addresses specified in the driver can overload our local generic functions

   Function * drivertable;
   if (!modDriver->get(FID_FunctionList, drivertable)) {
      for (int i=0; drivertable[i].Name; i++) {
         if (drivertable[i].Address) JumpTable[i].Address = drivertable[i].Address;
      }
   }

   return ERR_Okay;
}

static ERROR MODOpen(OBJECTPTR Module)
{
   Module->set(FID_FunctionList, JumpTable);
   return ERR_Okay;
}

static ERROR MODExpunge(void)
{
   if (modDriver) { FreeResource(modDriver); modDriver = nullptr; }
   return ERR_Okay;
}

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, MODOpen, MODExpunge, nullptr, MOD_IDL, nullptr)
