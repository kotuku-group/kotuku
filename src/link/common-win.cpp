#ifndef KOTUKU_STATIC

#include <string>
#include <cstdio>

extern "C" {
DLLCALL APTR WINAPI LoadLibraryA(CSTRING);
DLLCALL int WINAPI FreeLibrary(APTR);
DLLCALL int WINAPI FindClose(APTR);
DLLCALL APTR WINAPI FindFirstFileA(STRING, void *);
DLLCALL APTR WINAPI GetProcAddress(APTR, CSTRING);
DLLCALL int WINAPI RegOpenKeyExA(int,CSTRING,int,int,APTR *);
DLLCALL int WINAPI RegQueryValueExA(APTR,CSTRING,int *,int *, char *,int *);
DLLCALL void WINAPI CloseHandle(APTR);
DLLCALL int  WINAPI MessageBoxA(int,CSTRING,CSTRING,int);
DLLCALL int WINAPI GetCurrentDirectoryA(int, CSTRING);
DLLCALL int WINAPI GetModuleFileNameA(APTR, CSTRING, int);
DLLCALL int WINAPI SetDllDirectoryA(CSTRING);
DLLCALL int WINAPI SetDefaultDllDirectories(int DirectoryFlags);
DLLCALL void * AddDllDirectory(STRING NewDirectory);
DLLCALL int WINAPI WideCharToMultiByte(unsigned int, unsigned long, const wchar_t *, int, char *, int, const char *, int *);
}

#define CP_UTF8 65001

// DLL notification structures and function for hooking DLL loads
typedef struct _UNICODE_STRING {
   unsigned short Length;
   unsigned short MaximumLength;
   wchar_t *Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _LDR_DLL_LOADED_NOTIFICATION_DATA {
   unsigned long Flags;
   PUNICODE_STRING FullDllName;
   PUNICODE_STRING BaseDllName;
   void *DllBase;
   unsigned long SizeOfImage;
} LDR_DLL_LOADED_NOTIFICATION_DATA, *PLDR_DLL_LOADED_NOTIFICATION_DATA;

typedef struct _LDR_DLL_UNLOADED_NOTIFICATION_DATA {
   unsigned long Flags;
   PUNICODE_STRING FullDllName;
   PUNICODE_STRING BaseDllName;
   void *DllBase;
   unsigned long SizeOfImage;
} LDR_DLL_UNLOADED_NOTIFICATION_DATA, *PLDR_DLL_UNLOADED_NOTIFICATION_DATA;

typedef union _LDR_DLL_NOTIFICATION_DATA {
   LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
   LDR_DLL_UNLOADED_NOTIFICATION_DATA Unloaded;
} LDR_DLL_NOTIFICATION_DATA, *PLDR_DLL_NOTIFICATION_DATA;

#define LDR_DLL_NOTIFICATION_REASON_LOADED 1
#define LDR_DLL_NOTIFICATION_REASON_UNLOADED 2

typedef void (WINAPI *PLDR_DLL_NOTIFICATION_FUNCTION)(
   unsigned long NotificationReason,
   PLDR_DLL_NOTIFICATION_DATA NotificationData,
   void *Context
);

typedef long (WINAPI *PLdrRegisterDllNotification)(
   unsigned long Flags,
   PLDR_DLL_NOTIFICATION_FUNCTION NotificationFunction,
   void *Context,
   void **Cookie
);

#define HKEY_LOCAL_MACHINE 0x80000002
#define KEY_READ 0x20019
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE (void *)(-1)

typedef struct _FILETIME {
	int dwLowDateTime;
	int dwHighDateTime;
} FILETIME,*PFILETIME,*LPFILETIME;

typedef struct _WIN32_FIND_DATAW {
	int dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	int nFileSizeHigh;
	int nFileSizeLow;
	int dwReserved0;
	int dwReserved1;
   uint16_t cFileName[MAX_PATH];
	uint16_t cAlternateFileName[14];
} WIN32_FIND_DATAW,*LPWIN32_FIND_DATAW;

typedef WIN32_FIND_DATAW WIN32_FIND_DATA,*LPWIN32_FIND_DATA;

//********************************************************************************************************************
// DLL load notification callback - prints all DLL loads for debugging

#ifdef DEBUG_DLL_LOADS
static void WINAPI dll_notification_callback(unsigned long NotificationReason,
   PLDR_DLL_NOTIFICATION_DATA NotificationData, void *Context)
{
   if (NotificationReason IS LDR_DLL_NOTIFICATION_REASON_LOADED) {
      if (NotificationData and NotificationData->Loaded.FullDllName and
          NotificationData->Loaded.FullDllName->Buffer) {
         // Convert wide string to multibyte for proper printing
         char buffer[1024];
         int len = WideCharToMultiByte(CP_UTF8, 0, NotificationData->Loaded.FullDllName->Buffer, -1,
                                       buffer, sizeof(buffer), nullptr, nullptr);
         if (len > 0) {
            printf("[DLL LOAD] %s\n", buffer);
            fflush(stdout);
         }
      }
   }
}
#endif

//********************************************************************************************************************

static APTR find_core()
{
#ifdef DEBUG_DLL_LOADS
   // Register DLL load notification callback for debugging
   if (auto ntdll = LoadLibraryA("ntdll.dll")) {
      auto ldr_register = (PLdrRegisterDllNotification)GetProcAddress(ntdll, "LdrRegisterDllNotification");
      if (ldr_register) {
         void *dll_cookie = nullptr;
         ldr_register(0, dll_notification_callback, nullptr, &dll_cookie);
      }
   }
#endif

   std::string core_lib, folder;

   folder.clear();

   if (folder.empty()) {
      WIN32_FIND_DATA find;
      char buffer[MAX_PATH] = {};

      // Check local directories for base installation

      if (auto len = GetModuleFileNameA(nullptr, buffer, sizeof(buffer))) {
         for (int i = len; i > 0; i--) {
            if (buffer[i] IS '\\') {
               buffer[i+1] = 0;
               AddDllDirectory(buffer);
               break;
            }
         }
         folder = buffer;
      }

      // If GetModuleFileName() failed, try GetCurrentDirectory()

      if (folder.empty() and GetCurrentDirectoryA(sizeof(buffer), buffer)) {
         folder = buffer;
      }

      if (not folder.empty()) {
         auto test_lib = folder + "lib\\core.dll";
         auto handle = INVALID_HANDLE_VALUE;
         while ((handle = FindFirstFileA(test_lib.data(), &find)) IS INVALID_HANDLE_VALUE) {
            // Traverse one folder towards the root of the installation.

            if (folder.back() IS '\\') folder.pop_back();
            while ((not folder.empty()) and (folder.back() != '\\')) {
               folder.pop_back();
            }

            if (folder.empty()) break;  // No more parent directories

            test_lib = folder + "lib\\core.dll";
         }

         if (handle != INVALID_HANDLE_VALUE) { // Success in finding the core.dll file
            FindClose(handle);
            core_lib = test_lib;
         }
         else folder.clear();
      }
   }

   if (core_lib.empty()) { // If local core library not found, check the Windows registry
      APTR keyhandle;
      if (not RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Kotuku", 0, KEY_READ, &keyhandle)) {
         char buffer[MAX_PATH] = {};
         int j = sizeof(buffer);
         if (not RegQueryValueExA(keyhandle, "Location", 0, 0, buffer, &j)) {
            folder.assign(buffer);
            core_lib = folder + "lib\\core.dll";
         }
         CloseHandle(keyhandle);
         keyhandle = nullptr;
      }
   }

   // Prior to loading the core we must add the root and 3rdparty lib folder to the DLL search path
   // so that zlib, freetype and so forth are found.

   auto dll_folder = folder + "lib\\lib";
   SetDllDirectoryA(dll_folder.c_str());

   APTR handle;
   core_lib = folder + "lib\\core.dll";
   if (!(handle = LoadLibraryA(core_lib.c_str()))) return nullptr;
   return handle;
}

#endif // KOTUKU_STATIC
