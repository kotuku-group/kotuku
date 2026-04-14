#define PRV_FILESYSTEM

#ifdef _MSC_VER
#pragma warning (disable : 4244 4311 4312 4267 4244 4068) // Disable annoying VC++ warnings
#endif

#define _WIN32_WINNT 0x0600 // Required for CRITICAL_SECTION - min version. Windows Vista
#define NO_STRICT // Turn off type management due to C++ mangling issues.
#define PSAPI_VERSION 1

#include <stdio.h>
#include <wtypes.h>
#include <memory.h>
#include <winnt.h>
#include <winbase.h>
#include <psapi.h>
#include <io.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <locale.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <algorithm>
#ifndef NDEBUG
#include <crtdbg.h>
#endif
#ifdef _MSC_VER
 #include <io.h>
#else
 #include <unistd.h>
 #include <sys/time.h>
#endif
#include <winioctl.h>
#include <shlobj.h>

#ifdef __CYGWIN__
#include <sys/timespec.h>
#endif
#include <tchar.h>
#include <imagehlp.h>

#ifndef PLATFORM_CONFIG_H
#include <kotuku/config.h>
#endif

#include "windefs.h"
#include <kotuku/system/errors.h>

#define STD_TIMEOUT 1000

// Constants to replace magic numbers
constexpr long long PROCESS_MESSAGE_TIMEOUT_US = 100000;
constexpr int MAX_MODULE_PATH = 512;
constexpr int MAX_ERROR_MSG = 400;
constexpr int MAX_USERNAME = 256;
constexpr int MAX_ENV_VALUE = 512;

//#define MSG(...) printf(__VA_ARGS__)
#define MSG(...)

#include <string>
#include <array>
#include <chrono>

#define WAITLOCK_EVENTS 1 // Use events instead of semaphores for waitlocks (recommended)

enum {
   STAGE_STARTUP=1,
   STAGE_ACTIVE,
   STAGE_SHUTDOWN
};

extern "C" int glProcessID;
extern "C" HANDLE glProcessHandle;
extern "C" int8_t glProgramStage;

static HANDLE glInstance = nullptr;
static HANDLE glMsgWindow = nullptr;
HANDLE glValidationSemaphore = nullptr;

// Thread safety for global variables
static CRITICAL_SECTION csGlobalAccess;
static bool csGlobalInitialized = false;

#ifndef _MSC_VER
WINBASEAPI VOID WINAPI InitializeConditionVariable(PCONDITION_VARIABLE ConditionVariable);
WINBASEAPI WINBOOL WINAPI SleepConditionVariableCS(PCONDITION_VARIABLE ConditionVariable, PCRITICAL_SECTION CriticalSection, DWORD dwMilliseconds);
WINBASEAPI WINBOOL WINAPI SleepConditionVariableSRW(PCONDITION_VARIABLE ConditionVariable, PSRWLOCK SRWLock, DWORD dwMilliseconds, uint32_t Flags);
WINBASEAPI VOID WINAPI WakeAllConditionVariable(PCONDITION_VARIABLE ConditionVariable);
WINBASEAPI VOID WINAPI WakeConditionVariable(PCONDITION_VARIABLE ConditionVariable);
#endif


extern "C" ERR plAllocPrivateSemaphore(HANDLE *Semaphore, int InitialValue);
extern "C" void plFreePrivateSemaphore(HANDLE *Semaphore);

static LRESULT CALLBACK window_procedure(HWND, UINT, WPARAM, LPARAM);

extern "C" int validate_process(int ProcessID);

typedef void * APTR;
typedef unsigned char uint8_t;

static UINT glDeadProcessMsg; // Read only.

#define LEN_OUTPUTBUFFER 1024

int ExceptionFilter(LPEXCEPTION_POINTERS Args);
static BOOL break_handler(DWORD CtrlType);

static int (*glCrashHandler)(int, APTR, int, APTR) = nullptr;
static void (*glBreakHandler)(void) = nullptr;

struct stdpipe {
   HANDLE Read;
   HANDLE Write;
   HANDLE Event;
   OVERLAPPED OvRead;
   OVERLAPPED OvWrite;
   char Buffer[1024];
};

struct winprocess {
   APTR Task;
   HANDLE Handle;       // The handle to the created process
   struct stdpipe PipeOut;
   struct stdpipe PipeIn;
   struct stdpipe PipeErr;
   OVERLAPPED OutOverlap;
   OVERLAPPED ErrOverlap;
   HANDLE StdErrEvent;
   HANDLE StdOutEvent;
   char OutBuffer[4];
   char ErrBuffer[4];
   DWORD OutTotalRead;
   DWORD ErrTotalRead;
};

#define MAX_HANDLES 20

static struct {
   int   OtherProcess;
   HANDLE OtherHandle;
   HANDLE LocalHandle;
} glHandleBank[MAX_HANDLES];

static int16_t glHandleCount = 0;
static CRITICAL_SECTION csHandleBank;
static CRITICAL_SECTION csJob;


//typedef unsigned char * STRING;
typedef long long int64_t;
typedef void * APTR;
//typedef void * OBJECTPTR;
typedef unsigned char uint8_t;


typedef struct DateTime {
   int16_t Year;
   int8_t Month;
   int8_t Day;
   int8_t Hour;
   int8_t Minute;
   int8_t Second;
   int8_t TimeZone;
} DateTime;

#define DRIVETYPE_REMOVABLE 1
#define DRIVETYPE_CDROM     2
#define DRIVETYPE_FIXED     3
#define DRIVETYPE_NETWORK   4
#define DRIVETYPE_USB       5

#define MFF_READ 0x00000001
#define MFF_MODIFY 0x00000002
#define MFF_CREATE 0x00000004
#define MFF_DELETE 0x00000008
#define MFF_MOVED 0x00000010
#define MFF_ATTRIB 0x00000020
#define MFF_OPENED 0x00000040
#define MFF_CLOSED 0x00000080
#define MFF_UNMOUNT 0x00000100
#define MFF_FOLDER 0x00000200
#define MFF_FILE 0x00000400
#define MFF_SELF 0x00000800
#define MFF_DEEP 0x00001000
#define MFF_RENAME (MFF_MOVED)
#define MFF_WRITE (MFF_MODIFY)

// Return codes available to the feedback routine

enum class FFR : int {
   NIL = 0,
   OKAY = 0,
   CONTINUE = 0,
   SKIP = 1,
   ABORT = 2,
};


#define PERMIT_READ 0x00000001
#define PERMIT_WRITE 0x00000002
#define PERMIT_EXEC 0x00000004
#define PERMIT_DELETE 0x00000008
#define PERMIT_GROUP_READ 0x00000010
#define PERMIT_GROUP_WRITE 0x00000020
#define PERMIT_GROUP_EXEC 0x00000040
#define PERMIT_GROUP_DELETE 0x00000080
#define PERMIT_OTHERS_READ 0x00000100
#define PERMIT_OTHERS_WRITE 0x00000200
#define PERMIT_OTHERS_EXEC 0x00000400
#define PERMIT_OTHERS_DELETE 0x00000800
#define PERMIT_HIDDEN 0x00001000
#define PERMIT_ARCHIVE 0x00002000
#define PERMIT_PASSWORD 0x00004000
#define PERMIT_USERID 0x00008000
#define PERMIT_GROUPID 0x00010000
#define PERMIT_INHERIT 0x00020000
#define PERMIT_OFFLINE 0x00040000
#define PERMIT_NETWORK 0x00080000
#define PERMIT_USER_READ (PERMIT_READ)
#define PERMIT_USER_WRITE (PERMIT_WRITE)
#define PERMIT_USER_EXEC (PERMIT_EXEC)
#define PERMIT_EVERYONE_READ (PERMIT_READ|PERMIT_GROUP_READ|PERMIT_OTHERS_READ)
#define PERMIT_EVERYONE_WRITE (PERMIT_WRITE|PERMIT_GROUP_WRITE|PERMIT_OTHERS_WRITE)
#define PERMIT_EVERYONE_EXEC (PERMIT_EXEC|PERMIT_GROUP_EXEC|PERMIT_OTHERS_EXEC)
#define PERMIT_EVERYONE_DELETE (PERMIT_DELETE|PERMIT_GROUP_DELETE|PERMIT_OTHERS_DELETE)
#define PERMIT_ALL_READ (PERMIT_EVERYONE_READ)
#define PERMIT_ALL_WRITE (PERMIT_EVERYONE_WRITE)
#define PERMIT_ALL_EXEC (PERMIT_EVERYONE_EXEC)
#define PERMIT_ALL_DELETE (PERMIT_EVERYONE_DELETE)
#define PERMIT_EVERYONE_ACCESS (PERMIT_EVERYONE_READ|PERMIT_EVERYONE_WRITE|PERMIT_EVERYONE_EXEC|PERMIT_EVERYONE_DELETE)
#define PERMIT_EVERYONE_READWRITE (PERMIT_EVERYONE_READ|PERMIT_EVERYONE_WRITE)
#define PERMIT_USER (PERMIT_READ|PERMIT_WRITE|PERMIT_EXEC|PERMIT_DELETE)
#define PERMIT_GROUP (PERMIT_GROUP_READ|PERMIT_GROUP_WRITE|PERMIT_GROUP_EXEC|PERMIT_GROUP_DELETE)
#define PERMIT_OTHERS (PERMIT_OTHERS_READ|PERMIT_OTHERS_WRITE|PERMIT_OTHERS_EXEC|PERMIT_OTHERS_DELETE)

constexpr int LOC_DIRECTORY = 1;
constexpr int LOC_FOLDER = 1;
constexpr int LOC_VOLUME = 2;
constexpr int LOC_FILE = 3;

struct FileFeedback {
   int64_t Size;          // Size of the file
   int64_t Position;      // Current seek position within the file if moving or copying
   STRING  Path;
   STRING  Dest;          // Destination file/path if moving or copying
   int     FeedbackID;    // Set to one of the FDB integers
   char    Reserved[32];  // Reserved in case of future expansion
};

extern "C" FFR CALL_FEEDBACK(struct FUNCTION *, struct FileFeedback *);
extern "C" ERR convert_errno(int Error, ERR Default);
extern "C" int winReadPipe(HANDLE FD, APTR Buffer, DWORD *Size);
extern std::string winFormatMessage(int Error = GetLastError());

//********************************************************************************************************************

extern std::string winFormatMessage(int Error)
{
   std::string Buffer(MAX_ERROR_MSG, '\0');

   auto i = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, Error, 0, Buffer.data(), Buffer.size(), nullptr);
   while ((i > 0) and (Buffer[i-1] <= 0x20)) i--; // Windows puts whitespace at the end of error strings for some reason
   Buffer.resize(i);
   return Buffer;
}

static void printerror(void)
{
   LPVOID lpMsgBuf;
   FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
      GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR)&lpMsgBuf, 0, nullptr);
   fprintf(stderr, "WinError: %s", (LPTSTR)lpMsgBuf);
   LocalFree(lpMsgBuf);
}

//********************************************************************************************************************
// Console checker for Cygwin

int8_t is_console(HANDLE h)
{
   if (FILE_TYPE_UNKNOWN IS GetFileType(h) and ERROR_INVALID_HANDLE IS GetLastError()) {
       if ((h = CreateFile("CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr))) {
           CloseHandle(h);
           return true;
       }
   }

   CONSOLE_FONT_INFO cfi;
   return GetCurrentConsoleFont(h, false, &cfi) != 0;
}

//********************************************************************************************************************
// If the program is launched from a console, attach to it.  Otherwise create a new console window and redirect output
// to it (e.g. if launched from a desktop icon).

extern "C" void activate_console(int8_t AllowOpenConsole)
{
   static bool activated = false;

   if (!activated) {
      char value[8];
      if (GetEnvironmentVariable("TERM", value, sizeof(value)) or
          GetEnvironmentVariable("PROMPT", value, sizeof(value))) { // TERM defined by Cygwin, Mingw, PROMPT defined by cmd.exe

         // NB: Cygwin stdout/err handling is broken and requires the following workaround for ensuring that stdout
         // and stderr are managed correctly for both standard console output and file redirection.

         HANDLE current_out = GetStdHandle(STD_OUTPUT_HANDLE);
         HANDLE current_err = GetStdHandle(STD_ERROR_HANDLE);

         AttachConsole(ATTACH_PARENT_PROCESS);

         if (is_console(current_out)) freopen("CON", "w", stdout);  // Redirect stdout and stderr descriptors to the attached console.
         if (is_console(current_err)) freopen("CON", "w", stderr);
      }
      else if (AllowOpenConsole) { // Assume that executable was launched from desktop without a console
         AllocConsole();
         AttachConsole(GetCurrentProcessId());
         freopen("CON", "w", stdout);  // Redirect stdout and stderr descriptors to the attached console.
         freopen("CON", "w", stderr);
      }
      else return;

      // Set console mode to handle UTF-8 properly

      SetConsoleOutputCP(CP_UTF8);
      SetConsoleCP(CP_UTF8);

      // Set the console title to the program name
      char title[MAX_MODULE_PATH];
      ZeroMemory(title, sizeof(title));
      if (GetModuleFileName(nullptr, title, sizeof(title) - 1)) {
         title[sizeof(title) - 1] = 0; // Ensure null termination
         char* last_slash = strrchr(title, '\\');
         if (last_slash) {
            last_slash++; // Skip the slash
            // Remove file extension for cleaner title
            char* dot = strrchr(last_slash, '.');
            if (dot) *dot = 0;
         }
         else last_slash = title; // No path, use the whole string
         SetConsoleTitle(last_slash);
      }

      activated = true;
   }
}

//********************************************************************************************************************

static inline unsigned int LCASEHASH(const char* String) noexcept
{
   unsigned int hash = 5381;
   unsigned char c;
   while ((c = *String++)) {
      if ((c >= 'A') and (c <= 'Z')) {
         hash = (hash<<5) + hash + c - 'A' + 'a';
      }
      else hash = (hash<<5) + hash + c;
   }
   return hash;
}

//********************************************************************************************************************

#ifndef NDEBUG
static char glSymbolsLoaded = false;
static void windows_print_stacktrace(CONTEXT* context)
{
   if (!glSymbolsLoaded) return;

   STACKFRAME frame = { {0} };

   // setup initial stack frame
   #ifdef _LP64
      frame.AddrPC.Offset    = context->Rip;
      frame.AddrStack.Offset = context->Rsp;
      frame.AddrFrame.Offset = context->Rbp;
   #else
      frame.AddrPC.Offset    = context->Eip;
      frame.AddrStack.Offset = context->Esp;
      frame.AddrFrame.Offset = context->Ebp;
   #endif
   frame.AddrPC.Mode    = AddrModeFlat;
   frame.AddrStack.Mode = AddrModeFlat;
   frame.AddrFrame.Mode = AddrModeFlat;

   while (StackWalk(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), GetCurrentThread(), &frame, context, 0, SymFunctionTableAccess, SymGetModuleBase, 0)) {
      // Declare an image help symbol structure to hold symbol info and name up to 256 chars This struct is of variable length though so it must be declared as a raw byte buffer.

      static char symbolBuffer[sizeof(IMAGEHLP_SYMBOL) + 255];
      memset( symbolBuffer, 0, sizeof(IMAGEHLP_SYMBOL) + 255);

      IMAGEHLP_SYMBOL *symbol = (IMAGEHLP_SYMBOL *)symbolBuffer;
      symbol->SizeOfStruct  = sizeof(IMAGEHLP_SYMBOL) + 255;
      symbol->MaxNameLength = 254;

      #ifdef _LP64
         DWORD64 displacement = 0;
      #else
         DWORD displacement = 0;
      #endif
      if (SymGetSymFromAddr(GetCurrentProcess(), frame.AddrPC.Offset, &displacement, symbol)) {
         fprintf(stderr, "0x%p %s\n", (APTR)frame.AddrPC.Offset, symbol->Name);

         IMAGEHLP_LINE line;
         line.SizeOfStruct = sizeof(line);
         line.LineNumber = 0;
         DWORD line_displacement = 0;
         if (SymGetLineFromAddr(GetCurrentProcess(), frame.AddrPC.Offset, &line_displacement, &line)) {
            fprintf(stderr, "Line: %s, %d\n", line.FileName, (int)line.LineNumber);
         }
         else {
            //fprintf(stderr, "SymGetLineFromAddr(): %s\n", winFormatMessage(GetLastError()).c_str());
         }
      }
      else {
         fprintf(stderr, "0x%p\n", (APTR)frame.AddrPC.Offset);
      }
   }

   SymCleanup(GetCurrentProcess());
}
#endif

//********************************************************************************************************************

static const char *glMsgClass = "RKLMessageClass";

using BREAK_HANDLER = void (*)();

extern "C" ERR winInitialise(unsigned int *PathHash, BREAK_HANDLER BreakHandler)
{
   MEMORY_BASIC_INFORMATION mbiInfo;
   char path[255];
   int len;

   #ifndef NDEBUG
      // This is only needed if the application crashes and a stack trace is printed.
      SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
      if (SymInitialize(GetCurrentProcess(), 0, true)) glSymbolsLoaded = true;
      // These hooks prevent MSVC dialog boxes from appearing when an assert() is made.
      _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG );
      _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR );
   #endif

   // This turns off dialog boxes that Microsoft forces upon the user in certain situations (e.g. "No Disk in Drive"
   // when checking floppy drives).

   SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX); // SEM_NOGPFAULTERRORBOX

   // Calculate a unique hash from the Core's DLL path.  This hash can then be used for telling which Kōtuku
   // programs are using the same set of binaries.

   if (PathHash) {
      *PathHash = 0;

      SetLastError(ERROR_SUCCESS);
      if (VirtualQuery(LPCVOID(winInitialise), &mbiInfo, sizeof(mbiInfo))) {
         if ((len = GetModuleFileName((HINSTANCE)mbiInfo.AllocationBase, path, sizeof(path)))) {
            *PathHash = LCASEHASH(path);
         }
      }
   }

   // These commands turn off buffering, which results in all output being flushed.  This will cause noticeable
   // slowdown when debugging if you enable it.
   //setbuf(stdout, nullptr);
   //setbuf(stderr, nullptr);

   // Setup a handler for intercepting CTRL-C, CTRL-BREAK and segfaults.  This function is limited to Windows 2000 Professional and above.

   if (BreakHandler) {
      glBreakHandler = BreakHandler;
      SetConsoleCtrlHandler((PHANDLER_ROUTINE)&break_handler, true);
   }

   InitializeCriticalSection(&csHandleBank);
   InitializeCriticalSection(&csJob);

   // Initialize global access critical section
   if (!csGlobalInitialized) {
      InitializeCriticalSection(&csGlobalAccess);
      csGlobalInitialized = true;
   }

   // Register a blocking (message style) semaphore for signalling that process validation is required.

   if (plAllocPrivateSemaphore(&glValidationSemaphore, 1) != ERR::Okay) return ERR::SemaphoreOperation;

   glDeadProcessMsg = RegisterWindowMessage("RKL_DeadProcess");

   // Create a dummy window for receiving messages.

   WNDCLASSEX wx;
   ZeroMemory(&wx, sizeof(wx));
   wx.cbSize        = sizeof(WNDCLASSEX);
   wx.lpfnWndProc   = window_procedure;
   wx.hInstance     = glInstance;
   wx.lpszClassName = glMsgClass;
   if (RegisterClassEx(&wx)) {
      glMsgWindow = CreateWindowEx(0, glMsgClass, "Kotuku",
         0, // WS flags
         0, 0, // Coordinates
         CW_USEDEFAULT, CW_USEDEFAULT,
         HWND_MESSAGE, (HMENU)nullptr, glInstance, nullptr);
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Platform specific semaphore handling functions.

extern "C" ERR plAllocPrivateSemaphore(HANDLE *Semaphore, int InitialValue)
{
   SECURITY_ATTRIBUTES security = {
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = nullptr,
      .bInheritHandle = false
   };
   if (!(*Semaphore = CreateSemaphore(&security, 0, InitialValue, nullptr))) return ERR::SemaphoreOperation;
   else return ERR::Okay;
}

extern "C" void plFreePrivateSemaphore(HANDLE *Semaphore)
{
   if (Semaphore and *Semaphore) {
      CloseHandle(*Semaphore);
      *Semaphore = nullptr;
   }
}

//********************************************************************************************************************
// Broadcast a message saying that our process is dying.  Status should be 0 for an initial broadcast (closure is
// starting) and 1 if the process has finished and closed the Core cleanly.

extern "C" void winDeathBringer(int Status)
{
   static int last_status = -1;

   if (csGlobalInitialized) {
      EnterCriticalSection(&csGlobalAccess);
      if (Status > last_status) {
         last_status = Status;
         SendMessage(HWND_BROADCAST, glDeadProcessMsg, glProcessID, Status);
      }
      LeaveCriticalSection(&csGlobalAccess);
   }
   else {
      // Fallback if critical section not initialized
      if (Status > last_status) {
         last_status = Status;
         SendMessage(HWND_BROADCAST, glDeadProcessMsg, glProcessID, Status);
      }
   }
}

//********************************************************************************************************************

extern "C" int winIsDebuggerPresent(void)
{
   return IsDebuggerPresent();
}

//********************************************************************************************************************
// Remove all allocations here, called at the end of CloseCore()

extern "C" void winShutdown(void)
{
   if (glValidationSemaphore) plFreePrivateSemaphore(&glValidationSemaphore);

   if (glMsgWindow) { DestroyWindow(glMsgWindow); glMsgWindow = nullptr; }
   UnregisterClass(glMsgClass, glInstance);

   EnterCriticalSection(&csHandleBank);
      int16_t i;
      for (i=0; i < glHandleCount; i++) {
         if (glHandleBank[i].LocalHandle) CloseHandle(glHandleBank[i].LocalHandle);
      }
      glHandleCount = 0;
   LeaveCriticalSection(&csHandleBank);

   DeleteCriticalSection(&csHandleBank);
   DeleteCriticalSection(&csJob);

   // Cleanup global access critical section
   if (csGlobalInitialized) {
      DeleteCriticalSection(&csGlobalAccess);
      csGlobalInitialized = false;
   }
}

//********************************************************************************************************************
// Return a duplicate handle linked to some other process.  A cache is used so that re-duplication is minimised.
/*
static HANDLE handle_cache(int OtherProcess, HANDLE OtherHandle, BYTE *Free)
{
   HANDLE result = 0;
   HANDLE foreignprocess;
   WORD i;

   *Free = false;

   if ((OtherProcess IS glProcessID) or (!OtherProcess)) return OtherHandle;

   EnterCriticalSection(&csHandleBank);

      for (i=0; i < glHandleCount; i++) { // Return the handle if it is already registered.
         if ((glHandleBank[i].OtherProcess IS OtherProcess) and (glHandleBank[i].OtherHandle IS OtherHandle)) {
            result = glHandleBank[i].LocalHandle;
            LeaveCriticalSection(&csHandleBank);
            return result;
         }
      }

      if ((foreignprocess = OpenProcess(PROCESS_ALL_ACCESS, false, OtherProcess))) { // Duplicate the handle
         if ((DuplicateHandle(foreignprocess, OtherHandle, glProcessHandle, &result, 0, false, DUPLICATE_SAME_ACCESS))) {
            if (glHandleCount < MAX_HANDLES) { // Store the handle in the cache.
               i = glHandleCount++;
               glHandleBank[i].OtherProcess = OtherProcess;
               glHandleBank[i].OtherHandle  = OtherHandle;
               glHandleBank[i].LocalHandle  = result;
            }
            else *Free = true; // If the handle can't be cached, the caller needs to free it.
         }
         CloseHandle(foreignprocess);
      }

   LeaveCriticalSection(&csHandleBank);
   return result;
}
*/
//********************************************************************************************************************
// The SysLock() function uses these publicly accessible handles for synchronising Core processes.

extern "C" ERR alloc_public_waitlock(HANDLE *Lock, const char *Name)
{
   if (!Lock) return ERR::NullArgs;

#ifdef WAITLOCK_EVENTS
   HANDLE event = nullptr;

   if (Name) {
      if ((event = OpenEvent(SYNCHRONIZE|EVENT_MODIFY_STATE, false, Name))) {
         *Lock = event;
         return ERR::Okay;
      }
   }

   SECURITY_ATTRIBUTES sa = {
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = nullptr,
      .bInheritHandle = false
   };

   if ((event = CreateEvent(&sa, false, false, Name))) {
      *Lock = event;
      return ERR::Okay;
   }
   else return ERR::SystemCall;
#else
   SECURITY_ATTRIBUTES security;

   security.nLength = sizeof(SECURITY_ATTRIBUTES);
   security.lpSecurityDescriptor = nullptr;
   security.bInheritHandle = false;
   if ((*Lock = CreateSemaphore(&security, 0, 1, Name))) return ERR::Okay;
   else return ERR::SystemCall;
#endif
}

extern "C" void free_public_waitlock(HANDLE Lock) noexcept
{
   if (Lock) CloseHandle(Lock);
}

extern "C" ERR wake_waitlock(HANDLE Lock, int TotalSleepers) noexcept
{
   if (!Lock) return ERR::NullArgs;

   ERR error = ERR::Okay;

   #ifdef WAITLOCK_EVENTS
      while (TotalSleepers-- > 0) {
         if (!SetEvent(Lock)) {
            fprintf(stderr, "SetEvent() failed: %s\n", winFormatMessage(GetLastError()).c_str());
            error = ERR::SystemCall;
            break;
         }
      }
   #else
      int prev;
      if (!ReleaseSemaphore(Lock, 1, &prev)) error = ERR::SystemCall;
   #endif

   return error;
}

//********************************************************************************************************************

#ifdef __CYGWIN__
static int strnicmp(const char *s1, const char *s2, size_t n)
{
   for (; n > 0; s1++, s2++, --n) {
      unsigned char c1 = *s1;
      unsigned char c2 = *s2;
      if ((c1 >= 'A') or (c1 <= 'Z')) c1 = c1 - 'A' + 'a';
      if ((c2 >= 'A') or (c2 <= 'Z')) c2 = c2 - 'A' + 'a';

      if (c1 != c2) return ((*(unsigned char *)s1 < *(unsigned char *)s2) ? -1 : +1);
      else if (c1 IS '\0') return 0;
   }
   return 0;
}
#endif

//********************************************************************************************************************

extern "C" DWORD winGetExeDirectory(DWORD Length, LPSTR String)
{
   if (!String or Length < 4) return 0; // Need at least "C:\\" + null terminator

   int len, i;
   WCHAR **list;

   // Use GetModuleFileName() to get the path of our executable and then extract the folder location from that.
   ZeroMemory(String, Length);
   if ((len = GetModuleFileName(nullptr, String, Length - 1))) {
      String[Length - 1] = 0; // Ensure null termination
      char* last_slash = strrchr(String, '\\');
      if (last_slash) {
         *(last_slash + 1) = 0;
         return (DWORD)(last_slash - String + 1);
      }
      return len;
   }

   // Attempt to get our .exe path using CommandLinetoArgv()

   int args;
   if ((list = CommandLineToArgvW(GetCommandLineW(), &args))) {
      size_t converted;
      if (wcstombs_s(&converted, String, Length, list[0], Length - 1) IS 0) {
         String[Length - 1] = 0; // Ensure null termination
      }
      LocalFree(list);

      for (i=0; String[i]; i++);
      while (i > 0) {
         if ((String[i] IS '/') or (String[i] IS '\\')) {
            String[i+1] = 0;
            return i;
         }
         i--;
      }
   }

   // Windows has not prepended the path to the executable.  (Observed in Windows 7 64).  Try another method...

   if ((len = GetProcessImageFileNameA(GetCurrentProcess(), String, Length)) > 0) {
      char tmp[MAX_PATH] = "";

      if (GetLogicalDriveStrings(sizeof(tmp)-1, tmp)) {
         char devname[MAX_PATH];
         char szDrive[3] = " :";
         char *p = tmp;

         do {
            szDrive[0] = p[0];

            if (QueryDosDevice(szDrive, devname, sizeof(devname))) {
               int devlen = strlen(devname);
               if (strnicmp(String, devname, devlen) IS devlen) {
                  if (String[devlen] IS '\\') {
                     // Replace device path with DOS path
                     std::string tmpfile = szDrive + std::string(String+devlen);
                     if ((tmpfile.size() > 0) and (tmpfile.size() < MAX_PATH)) {
                        size_t copy_len = std::min<size_t>(tmpfile.size(), Length - 1);
                        memcpy(String, tmpfile.c_str(), copy_len);
                        String[copy_len] = 0;
                        return copy_len;
                     }
                     else return 0; // Error in formatting
                  }
               }
            }

            while (*p++);
         } while (p[0]);
      }
   }

   // Last resort - use the current folder

   return GetCurrentDirectory(Length, String);
}

//********************************************************************************************************************
// Warning: DispatchMessage() can hang for modal operations.  An example of a modal operation is window resizing.  If
// necessary, this issue can be subverted with threading.
//
// See also: display/windows.c

extern "C" void winProcessMessages(void)
{
   MSG msg;
   auto start_time = std::chrono::steady_clock::now();
   auto timeout = std::chrono::microseconds(PROCESS_MESSAGE_TIMEOUT_US);
   ZeroMemory(&msg, sizeof(msg));
   while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
      if (std::chrono::steady_clock::now() - start_time > timeout) break; // This timer-break prevents any chance of infinite looping
   }
}

//********************************************************************************************************************

extern "C" void winLowerPriority(void) noexcept
{
   SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

//********************************************************************************************************************

extern "C" int winSetProcessPriority(int Priority) noexcept
{
   // Map Kōtuku priority values to Windows priority classes
   // Kōtuku uses: negative = lower priority, positive = higher priority, 0 = normal
   DWORD priorityClass;

   if (Priority <= -20) priorityClass = IDLE_PRIORITY_CLASS;              // Lowest priority
   else if (Priority <= -10) priorityClass = BELOW_NORMAL_PRIORITY_CLASS; // Below normal
   else if (Priority < 10) priorityClass = NORMAL_PRIORITY_CLASS;         // Normal priority (default)
   else if (Priority < 20) priorityClass = ABOVE_NORMAL_PRIORITY_CLASS;   // Above normal
   else priorityClass = HIGH_PRIORITY_CLASS;                              // High priority

   // Set the priority class for the current process
   if (SetPriorityClass(GetCurrentProcess(), priorityClass)) return 0; // Success
   else return GetLastError();
}

//********************************************************************************************************************

extern "C" int winGetProcessPriority(void) noexcept
{
   // Get current process priority class and map to Kōtuku priority values
   const DWORD priorityClass = GetPriorityClass(GetCurrentProcess());
   if (priorityClass IS 0) return -1; // Error occurred

   // Map Windows priority classes to Kōtuku values
   switch (priorityClass) {
      case IDLE_PRIORITY_CLASS:         return -20;  // Lowest priority
      case BELOW_NORMAL_PRIORITY_CLASS: return -10;  // Below normal
      case NORMAL_PRIORITY_CLASS:       return 0;    // Normal priority (default)
      case ABOVE_NORMAL_PRIORITY_CLASS: return 10;   // Above normal
      case HIGH_PRIORITY_CLASS:         return 15;   // High priority
      case REALTIME_PRIORITY_CLASS:     return 20;   // Realtime (highest)
      default:                          return 0;    // Default to normal if unknown
   }
}

extern "C" int64_t winGetProcessAffinityMask(void) noexcept
{
   DWORD_PTR processAffinityMask = 0;
   DWORD_PTR systemAffinityMask = 0;

   // Get current process affinity mask
   if (GetProcessAffinityMask(GetCurrentProcess(), &processAffinityMask, &systemAffinityMask)) {
      return (int64_t)processAffinityMask;
   }
   else {
      return 0; // Error - return 0 to indicate failure
   }
}

extern "C" int winSetProcessAffinityMask(int64_t AffinityMask) noexcept
{
   // Set CPU affinity mask for the current process
   // AffinityMask is a bitmask where each bit represents a CPU core
   // Bit 0 = Core 0, Bit 1 = Core 1, etc.

   if (AffinityMask IS 0) return ERROR_INVALID_PARAMETER; // Invalid mask

   DWORD_PTR processAffinityMask = (DWORD_PTR)AffinityMask;
   DWORD_PTR systemAffinityMask;

   // Get system affinity mask to validate our request
   if (not GetProcessAffinityMask(GetCurrentProcess(), &processAffinityMask, &systemAffinityMask)) {
      return GetLastError();
   }

   // Ensure requested mask is valid for this system
   DWORD_PTR requestedMask = (DWORD_PTR)AffinityMask;
   if ((requestedMask & systemAffinityMask) != requestedMask) {
      return ERROR_INVALID_PARAMETER; // Requested cores not available
   }

   // Set the process affinity mask
   if (SetProcessAffinityMask(GetCurrentProcess(), requestedMask)) return 0; // Success
   else return GetLastError();
}

//********************************************************************************************************************

extern "C" int winGetCurrentThreadId(void)
{
   return GetCurrentThreadId();
}

//********************************************************************************************************************
// A null-terminator will be included in the output buffer.  Returns 0 on error.

extern "C" DWORD winGetCurrentDirectory(DWORD Length, LPSTR String)
{
   return GetCurrentDirectory(Length, String);
}

//********************************************************************************************************************
// Checks if a process exists by attempting to open it.

extern "C" int winCheckProcessExists(DWORD ProcessID)
{
   HANDLE process;

   if ((process = OpenProcess(STANDARD_RIGHTS_REQUIRED, false, ProcessID))) {
      CloseHandle(process);
      return 1;
   }
   else return 0;
}

//********************************************************************************************************************

extern "C" int winFreeLibrary(HMODULE Module)
{
   return FreeLibrary(Module);
}

//********************************************************************************************************************

extern "C" HANDLE winLoadLibrary(LPCSTR Name)
{
   HANDLE h = LoadLibraryExA(Name, nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR|LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_USER_DIRS|LOAD_LIBRARY_SEARCH_SYSTEM32);
   return h;
}

//********************************************************************************************************************

extern "C" FARPROC winGetProcAddress(HMODULE Module, LPCSTR Name)
{
   if (!Module) return GetProcAddress(GetModuleHandle(nullptr), Name);
   else return GetProcAddress(Module, Name);
}

//********************************************************************************************************************

extern "C" HANDLE winGetCurrentProcess(void)
{
   return GetCurrentProcess();
}

//********************************************************************************************************************

extern "C" int winGetCurrentProcessId(void)
{
   return GetCurrentProcessId();
}

//********************************************************************************************************************

extern "C" size_t winGetProcessMemoryUsage(int ProcessID)
{
   PROCESS_MEMORY_COUNTERS pmc;
   HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, ProcessID);
   if (process) {
      if (GetProcessMemoryInfo(process, &pmc, sizeof(pmc))) {
         CloseHandle(process);
         return pmc.WorkingSetSize; // Return the working set size in bytes
      }
      CloseHandle(process);
   }
   return -1; // Failed to retrieve memory usage
}

//********************************************************************************************************************

static bool glConsoleMode = true; // Assume running from a terminal by default.
static HANDLE glCachedStdInput = nullptr; // Cache stdin handle

extern "C" int winReadStdInput(HANDLE FD, APTR Buffer, DWORD BufferSize, DWORD *Size)
{
   *Size = 0;

   if ((glConsoleMode) and (ReadConsoleA(GetStdHandle(STD_INPUT_HANDLE), Buffer, BufferSize, Size, nullptr))) {
      return 0; // Read at least 1 character
   }

   *Size = BufferSize;
   return winReadPipe(FD, Buffer, Size);
}

//********************************************************************************************************************

extern "C" HANDLE winGetStdInput(void)
{
   if (!glCachedStdInput) {
      glCachedStdInput = GetStdHandle(STD_INPUT_HANDLE);
      if (glCachedStdInput and !SetConsoleMode(glCachedStdInput, ENABLE_PROCESSED_INPUT)) {
         glConsoleMode = false;
      }
   }
   return glCachedStdInput;
}

//********************************************************************************************************************
// To be used on local handles only.

extern "C" int winWaitForSingleObject(HANDLE Handle, int Time)
{
   if (Time IS -1) Time = INFINITE;

   int result = WaitForSingleObject(Handle, Time);

   if (result IS WAIT_OBJECT_0) return 0;
   else if (result IS WAIT_TIMEOUT) return 1;
   else if (result IS WAIT_ABANDONED) return 2; // WAIT_ABANDONED means that the wait was successful, but indicates that the previous process that held the lock crashed before releasing it.
   else return 3;
}

//********************************************************************************************************************
// Return Values:
//   -1 = Timeout
//   -2 = A handle has been abandoned/freed/is invalid.  Handles[0] will be updated to reflect the bad handle.
//   -3 = A message was received in the windows message queue.
//   -4 = Unknown result returned from windows.

extern "C" int winWaitForObjects(int Total, HANDLE *Handles, int Time, int8_t WinMsgs)
{
   if (Time IS -1) Time = INFINITE;

   int input_flags = WinMsgs ? (QS_INPUT|QS_POSTMESSAGE|QS_TIMER|QS_PAINT|QS_HOTKEY|QS_SENDMESSAGE) : 0;

   auto result = MsgWaitForMultipleObjects(Total, Handles, false, Time, input_flags);

   if (result IS WAIT_TIMEOUT) return -1;
   else if ((result >= WAIT_ABANDONED_0) and (result < WAIT_ABANDONED_0+Total)) {
      Handles[0] = Handles[result - WAIT_ABANDONED_0];
      return -2; // One of the handles has been abandoned (freed)
   }
   else if ((result >= WAIT_OBJECT_0) and (result < WAIT_OBJECT_0+Total)) {
      return result - WAIT_OBJECT_0; // Result is the index of the signalled handle
   }
   else if (result IS WAIT_OBJECT_0+Total) {
      return -3; // A message was received in the windows message queue
   }
   else {
      if (auto error = GetLastError();error IS ERROR_INVALID_HANDLE) {
         Handles[0] = 0; // Find out which handle is to blame
         for (int i=0; i < Total; i++) {
            if (MsgWaitForMultipleObjects(1, Handles+i, false, 1, (WinMsgs) ? QS_ALLINPUT : 0) IS result) {
               if (GetLastError() IS ERROR_INVALID_HANDLE) {
                  Handles[0] = Handles[i];
                  break;
               }
            }
         }
         return -2;
      }
      else {
         fprintf(stderr, "MsgWaitForMultipleObjects(%d) result: %d, error: %s\n", (int)Total, (int)result, winFormatMessage(error).c_str());
         return -4;
      }
   }
}

//********************************************************************************************************************
// Designed for reading from pipes.  Returns -1 on general error, -2 if the pipe is broken, e.g. child process is dead.

extern "C" int winReadPipe(HANDLE FD, APTR Buffer, DWORD *Size)
{
   // Check if there is data available on the pipe

   DWORD avail = 0;
   if (!PeekNamedPipe(FD, nullptr, 0, nullptr, &avail, nullptr)) {
      *Size = 0;
      if (GetLastError() IS ERROR_BROKEN_PIPE) return -2;
      else return -1;
   }

   if (!avail) {
      *Size = 0;
      return 0;
   }

   if (ReadFile(FD, Buffer, *Size, Size, 0)) {
      return 0; // Success
   }
   else {
      *Size = 0;
      if (GetLastError() IS ERROR_BROKEN_PIPE) return -2;
      else return -1;
   }
}

//********************************************************************************************************************
// WARNING: If it is not possible to write all data to the pipe, windows will block until the other side of the pipe
// has been read.  This does not match expected/documented functionality but there is no simple way to implement
// non-blocking anonymous pipes on windows.

extern "C" int winWritePipe(HANDLE FD, APTR Buffer, DWORD *Size)
{
   if (WriteFile(FD, Buffer, *Size, Size, 0)) {
      return 0; // Success
   }
   else {
      if (GetLastError() IS ERROR_BROKEN_PIPE) return -2;
      else return -1;
   }
}

//********************************************************************************************************************
// Used by class_thread.c only.

extern "C" ERR winCreatePipe(HANDLE *Read, HANDLE *Write)
{
   SECURITY_ATTRIBUTES sa;

   sa.nLength = sizeof(sa);
   sa.lpSecurityDescriptor = nullptr;
   sa.bInheritHandle = false;

   if (CreatePipe(Read, Write, &sa, 0)) {
      return ERR::Okay;
   }
   else return ERR::SystemCall;
}

//********************************************************************************************************************
// Returns zero on failure.

extern "C" int winCloseHandle(HANDLE Handle)
{
   if (Handle IS (HANDLE)-1) return 1;
   return CloseHandle(Handle);
}

//********************************************************************************************************************
// Returns zero on failure.

extern "C" int winUnmapViewOfFile(void *Address)
{
   return UnmapViewOfFile(Address);
}

//********************************************************************************************************************

extern "C" size_t winGetFileSize(char *Path)
{
   WIN32_FIND_DATA find;
   HANDLE handle;

   if ((handle = FindFirstFile(Path, &find)) IS INVALID_HANDLE_VALUE) {
      return 0;
   }

   auto size = (find.nFileSizeHigh * (MAXDWORD+1)) + find.nFileSizeLow;

   FindClose(handle);
   return size;
}

//********************************************************************************************************************

HANDLE glMemoryPool;

extern "C" int winCreateSharedMemory(char *Name, int mapsize, int initial_size, HANDLE *ControlID, void **Address)
{
   if (!ControlID or !Address or initial_size <= 0) return -3; // Invalid arguments

   *ControlID = nullptr;
   *Address = nullptr;

   // Create the shared memory area with proper security attributes
   SECURITY_ATTRIBUTES sa = {
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = nullptr,
      .bInheritHandle = false
   };

   if ((*ControlID = CreateFileMapping((HANDLE)-1, &sa, PAGE_READWRITE, 0, initial_size, Name))) {
      glMemoryPool = *ControlID;
      int init = (GetLastError() != ERROR_ALREADY_EXISTS) ? 1 : 0;
      if ((*Address = MapViewOfFile(*ControlID, FILE_MAP_WRITE, 0, 0, initial_size))) {
         return init;
      }
      else {
         CloseHandle(*ControlID);
         *ControlID = nullptr;
         return -2;
      }
   }
   else return -1;
}

//********************************************************************************************************************
// Allocate memory with OS-level protection using VirtualAlloc

extern "C" void * winAllocProtectedMemory(size_t Size, int ProtectionFlags)
{
   if (Size IS 0) return nullptr;

   // The caller must provide a page-aligned size (see align_page_size).
   // No need to realign here.

   // Determine protection flags from MEM flags
   DWORD protect = PAGE_NOACCESS;
   if (ProtectionFlags IS 0x00030000) protect = PAGE_READWRITE;  // MEM::READ_WRITE
   else if (ProtectionFlags & 0x00020000) protect = PAGE_READWRITE;  // MEM::WRITE
   else if (ProtectionFlags & 0x00010000) protect = PAGE_READONLY;   // MEM::READ

   return VirtualAlloc(nullptr, Size, MEM_COMMIT | MEM_RESERVE, protect);
}

//********************************************************************************************************************
// Free memory allocated with VirtualAlloc

extern "C" int winFreeProtectedMemory(void *Address, size_t Size)
{
   if (!Address) return 0;
   // VirtualFree with MEM_RELEASE ignores the size parameter and releases the entire region
   return VirtualFree(Address, 0, MEM_RELEASE) ? 1 : 0;
}

//********************************************************************************************************************
// Get the system page size

extern "C" size_t winGetPageSize(void)
{
   SYSTEM_INFO si;
   GetSystemInfo(&si);
   return si.dwPageSize;
}

//********************************************************************************************************************
// Change memory protection flags on an existing VirtualAlloc allocation

extern "C" int winProtectMemory(void *Address, size_t Size, bool Read, bool Write, bool Exec)
{
   if ((not Address) or (Size == 0)) return 0;

   DWORD protect = PAGE_NOACCESS;
   if (Write and Exec) protect = PAGE_EXECUTE_READWRITE;
   else if (Write) protect = PAGE_READWRITE;
   else if (Read and Exec) protect = PAGE_EXECUTE_READ;
   else if (Read) protect = PAGE_READONLY;
   else if (Exec) protect = PAGE_EXECUTE;

   DWORD old_protect;
   return VirtualProtect(Address, Size, protect, &old_protect) ? 1 : 0;
}

//********************************************************************************************************************

extern "C" int winDeleteFile(const char *Path)
{
   return DeleteFile(Path);
}

//********************************************************************************************************************

extern "C" void winGetEnv(const char *Name, std::string &Buffer)
{
   Buffer.clear();
   if (!Name) return;
   char buffer[4096];
   int result = GetEnvironmentVariable(Name, buffer, sizeof(buffer));
   if (result > 0) Buffer.assign(buffer, result);
}

//********************************************************************************************************************

extern "C" int winSetEnv(const char *Name, const char *Value)
{
   return SetEnvironmentVariable(Name, Value);
}

//********************************************************************************************************************

extern "C" void winTerminateThread(HANDLE Handle)
{
   TerminateThread(Handle, 0);
}

//********************************************************************************************************************

extern "C" ERR winWaitThread(HANDLE Handle, int TimeOut)
{
   if (WaitForSingleObject(Handle, TimeOut) IS WAIT_TIMEOUT) return ERR::TimeOut;
   else return ERR::Okay;
}

//********************************************************************************************************************

static BOOL break_handler(DWORD CtrlType)
{
   if (glBreakHandler) glBreakHandler();

   return false;
}

//********************************************************************************************************************

extern "C" void winSetUnhandledExceptionFilter(int (*Function)(int, APTR, int, APTR))
{
   if (Function) glCrashHandler = Function;
   else if (!glCrashHandler) return;  // If we're set with nullptr and no crash handler already exists, do not set or change the exception filter.
   SetUnhandledExceptionFilter((LPTOP_LEVEL_EXCEPTION_FILTER)&ExceptionFilter);
}

//********************************************************************************************************************
// https://docs.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record

int ExceptionFilter(LPEXCEPTION_POINTERS Args)
{
   int continuable, code, err;

   #ifndef NDEBUG
   if (Args->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
      windows_print_stacktrace(Args->ContextRecord);
   }
   #endif

   if (Args->ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE) {
      continuable = false;
   }
   else continuable = true;

   if (Args->ExceptionRecord->ExceptionCode IS EXCEPTION_NONCONTINUABLE_EXCEPTION) return EXCEPTION_CONTINUE_SEARCH;

   switch(Args->ExceptionRecord->ExceptionCode) {
      case EXCEPTION_ACCESS_VIOLATION:       code = EXP_ACCESS_VIOLATION; break;
      case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:  code = EXP_ACCESS_VIOLATION; break;
      case EXCEPTION_BREAKPOINT:             code = EXP_BREAKPOINT; break;
      case EXCEPTION_DATATYPE_MISALIGNMENT:  code = EXP_MISALIGNED_DATA; break;
      case EXCEPTION_FLT_DENORMAL_OPERAND:   code = EXP_INVALID_CALCULATION; break;
      case EXCEPTION_FLT_DIVIDE_BY_ZERO:     code = EXP_DIVIDE_BY_ZERO; break;
      case EXCEPTION_FLT_INEXACT_RESULT:     code = EXP_INVALID_CALCULATION; break;
      case EXCEPTION_FLT_INVALID_OPERATION:  code = EXP_INVALID_CALCULATION; break;
      case EXCEPTION_FLT_OVERFLOW:           code = EXP_INVALID_CALCULATION; break;
      case EXCEPTION_FLT_STACK_CHECK:        code = EXP_INVALID_CALCULATION; break;
      case EXCEPTION_FLT_UNDERFLOW:          code = EXP_INVALID_CALCULATION; break;
      case EXCEPTION_ILLEGAL_INSTRUCTION:    code = EXP_ILLEGAL_INSTRUCTION; break;
      case EXCEPTION_IN_PAGE_ERROR:          code = EXP_ACCESS_VIOLATION; break;
      case EXCEPTION_INT_DIVIDE_BY_ZERO:     code = EXP_DIVIDE_BY_ZERO; break;
      case EXCEPTION_INT_OVERFLOW:           code = EXP_INVALID_CALCULATION; break;
      case EXCEPTION_PRIV_INSTRUCTION:       code = EXP_ILLEGAL_INSTRUCTION; break;
      case EXCEPTION_STACK_OVERFLOW:         code = EXP_STACK_OVERFLOW; break;
      default: code = 0;
   }

   if (glCrashHandler) {
      err = glCrashHandler(code, Args->ExceptionRecord->ExceptionAddress, continuable, Args->ExceptionRecord->ExceptionInformation);

      if (err IS 0) return EXCEPTION_CONTINUE_EXECUTION;
      else if (err IS 1) return EXCEPTION_CONTINUE_SEARCH;
      else return EXCEPTION_EXECUTE_HANDLER;
   }
   else return EXCEPTION_EXECUTE_HANDLER;
}

// Sockets can use select() to differentiate between read/write states

extern "C" void winSelect(int FD, char *Read, char *Write)
{
   fd_set fread, fwrite;
   struct timeval tv;

   FD_ZERO(&fread);
   FD_ZERO(&fwrite);

   if (*Read) FD_SET(FD, &fread);
   if (*Write) FD_SET(FD, &fwrite);

   tv.tv_sec = 0;
   tv.tv_usec = 0;
   select(0, &fread, &fwrite, 0, &tv);

   *Read = FD_ISSET(FD, &fread);
   *Write = FD_ISSET(FD, &fwrite);
}

static BOOL CALLBACK TerminateAppEnum( HWND hwnd, LPARAM lParam ) ;

extern "C" ERR winTerminateApp(int dwPID, int dwTimeout)
{
   HANDLE hProc ;
   ERR dwRet;

   // If we can't open the process with PROCESS_TERMINATE rights, then we give up immediately.

   hProc = OpenProcess(SYNCHRONIZE|PROCESS_TERMINATE, false, dwPID);

   if (hProc IS nullptr) return ERR::ProcessCreation;

   // TerminateAppEnum() posts WM_CLOSE to all windows whose PID matches your process's.

   EnumWindows((WNDENUMPROC)TerminateAppEnum, (LPARAM) dwPID) ;

   // Wait on the handle. If it signals, great. If it times out, then you kill it.

   if (WaitForSingleObject(hProc, dwTimeout) != WAIT_OBJECT_0) {
      dwRet = (TerminateProcess(hProc,0) ? ERR::Okay : ERR::ProcessCreation);
   }
   else dwRet = ERR::Okay;

   CloseHandle(hProc);
   return dwRet;
}

static BOOL CALLBACK TerminateAppEnum( HWND hwnd, LPARAM lParam )
{
   DWORD dwID;

   GetWindowThreadProcessId(hwnd, &dwID);

   if (dwID IS (DWORD)lParam) {
      PostMessage(hwnd, WM_CLOSE, 0, 0);
   }

   return true;
}

//********************************************************************************************************************

extern int ProcessMessages(int Flags, int Timeout);

static LRESULT CALLBACK window_procedure(HWND window, UINT msgcode, WPARAM wParam, LPARAM lParam)
{
   if (glProgramStage IS STAGE_SHUTDOWN) return DefWindowProc(window, msgcode, wParam, lParam);

   if (msgcode IS glDeadProcessMsg) {
      validate_process(wParam);
      return 0;
   }
   else return DefWindowProc(window, msgcode, wParam, lParam);
}

//********************************************************************************************************************

#include "processes.cpp"

//********************************************************************************************************************
// Called on file system initialisation to create assignments specific to windows.

struct spec_folder {
   int id;
   std::string assign;
   std::string label;
   std::string icon;
   char hidden;
};

extern "C" void winEnumSpecialFolders(void (*enumfolder)(const char *, const char *, const char *, const char *, char))
{
   static const spec_folder folders[] = {
      //CSIDL_NETWORK
      //CSIDL_PRINTHOOD
      //CSIDL_DESKTOPDIRECTORY
      //{ CSIDL_PRINTERS, "printers:",  "Printers",        "devices/printer" },
      //{ CSIDL_DRIVES,   "computer:",  "My Computer",     "programs/filemanager" }
      { CSIDL_NETHOOD,  "network:",   "Network Places", "devices/network", 0 },
      { CSIDL_PERSONAL, "documents:", "Documents",      "page/multiple", 0 },
      { CSIDL_DESKTOPDIRECTORY, "desktop:", "Desktop",  "devices/harddisk", 0 }
   };
   char path[MAX_PATH];

   for (unsigned i=0; i < sizeof(folders) / sizeof(folders[0]); i++) {
      if (SHGetFolderPath(nullptr, folders[i].id, nullptr, 0, path) IS S_OK) {
         enumfolder(folders[i].assign.c_str(), folders[i].label.c_str(), path, folders[i].icon.c_str(), folders[i].hidden);
      }
   }

   if (GetTempPath(sizeof(path), path) < sizeof(path)) {
      enumfolder("HostTemp:", "Temp", path, "items/trash", 1);
   }
}

//********************************************************************************************************************

extern "C" int winGetFullPathName(const char *Path, int PathLength, char *Output, char **NamePart)
{
   return GetFullPathName(Path, PathLength, Output, NamePart);
}

//********************************************************************************************************************

extern "C" int8_t winGetCommand(char *Path, char *Buffer, int BufferSize)
{
   if (BufferSize < MAX_PATH+3) return 1;

   HINSTANCE result = FindExecutable(Path, nullptr, Buffer+1);
   if ((result > (HINSTANCE)32) and (Buffer[1])) { /* Success */
      *Buffer++ = '"';
      while (*Buffer) Buffer++;
      *Buffer++ = '"';
      *Buffer = 0;
      return 0;
   }
   else return 1;
}

//********************************************************************************************************************

extern "C" int winCurrentDirectory(char *Buffer, int BufferSize)
{
   Buffer[0] = 0;
   if (auto len = GetModuleFileNameA(nullptr, Buffer, BufferSize)) {
      for (auto i=len; i > 0; i--) {
         if (Buffer[i] IS '\\') {
            Buffer[i+1] = 0;
            break;
         }
      }
   }

   // If GetModuleFileName() failed, try GetCurrentDirectory()

   if (!Buffer[0]) GetCurrentDirectoryA(BufferSize, Buffer);

   if (Buffer[0]) return 1;
   else return 0;
}

//********************************************************************************************************************

static void convert_time(FILETIME *Source, struct DateTime *Dest)
{
   FILETIME filetime;
   SYSTEMTIME time;

   FileTimeToLocalFileTime(Source, &filetime);
   if (FileTimeToSystemTime(&filetime, &time)) {
      Dest->Year   = time.wYear;
      Dest->Month  = time.wMonth;
      Dest->Day    = time.wDay;
      Dest->Hour   = time.wHour;
      Dest->Minute = time.wMinute;
      Dest->Second = time.wSecond;
   }
}

//********************************************************************************************************************

extern "C" ERR winGetFileAttributesEx(const char *Path, int8_t *Hidden, int8_t *ReadOnly, int8_t *Archive, int8_t *Folder, int64_t *Size,
   struct DateTime *LastWrite, struct DateTime *LastAccess, struct DateTime *LastCreate)
{
   WIN32_FILE_ATTRIBUTE_DATA info;

   if (!GetFileAttributesEx(Path, GetFileExInfoStandard, &info)) return ERR::SystemCall;

   if (info.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) *Hidden = true;
   else *Hidden = false;

   if (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) *ReadOnly = true;
   else *ReadOnly = false;

   if (info.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE) *Archive = true;
   else *Archive = false;

   if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      *Folder = true;
      *Size = 0;
   }
   else {
      *Folder = false;
      *Size = (info.nFileSizeHigh * (MAXDWORD+1)) + info.nFileSizeLow;
   }

   if (LastWrite) convert_time(&info.ftLastWriteTime, LastWrite);
   if (LastAccess) convert_time(&info.ftLastAccessTime, LastAccess);
   if (LastCreate) convert_time(&info.ftCreationTime, LastCreate);

   return ERR::Okay;
}

//********************************************************************************************************************

extern "C" ERR winCreateDir(const char *Path)
{
   if (auto result = CreateDirectory(Path, nullptr)) return ERR::Okay;
   else {
      result = GetLastError();
      if (result IS ERROR_ALREADY_EXISTS) return ERR::FileExists;
      else if (result IS ERROR_PATH_NOT_FOUND) return ERR::FileNotFound;
      else return ERR::SystemCall;
   }
}

//********************************************************************************************************************
// Returns true on success.

extern "C" int winGetFreeDiskSpace(char Drive, long long *TotalSpace, long long *BytesUsed)
{
   DWORD sectors, bytes_per_sector, free_clusters, total_clusters;

   *TotalSpace = 0;
   *BytesUsed = 0;

   char location[4] = { Drive, ':', '\\', 0 };

   if (GetDiskFreeSpace(location, &sectors, &bytes_per_sector, &free_clusters, &total_clusters)) {
      *TotalSpace = (double)sectors * (double)bytes_per_sector * (double)free_clusters;
      *BytesUsed  = ((double)sectors * (double)bytes_per_sector * (double)total_clusters);
      return 1;
   }
   else return 0;
}

//********************************************************************************************************************
// This function retrieves the original Creation date of the file and applies it to the Modification and Access date/times.

extern "C" int winResetDate(STRING Location)
{
   HANDLE handle;

   if ((handle = CreateFile(Location, GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr,
         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) != INVALID_HANDLE_VALUE) {

      FILETIME filetime;
      GetFileTime(handle, &filetime, nullptr, nullptr);
      int err = SetFileTime(handle, nullptr, &filetime, &filetime);
      CloseHandle(handle);
      if (err) return 1;
   }

   return 0;
}

//********************************************************************************************************************

extern "C" void winFindNextChangeNotification(HANDLE Handle)
{
   FindNextChangeNotification(Handle);
}

//********************************************************************************************************************

extern "C" void winFindCloseChangeNotification(HANDLE Handle)
{
   FindCloseChangeNotification(Handle);
}

//********************************************************************************************************************

extern "C" int winGetWatchBufferSize(void)
{
   return sizeof(OVERLAPPED) + sizeof(FILE_NOTIFY_INFORMATION) + MAX_PATH;
}

//********************************************************************************************************************

extern "C" int winValidateHandle(HANDLE Handle)
{
   if (!Handle or (Handle IS INVALID_HANDLE_VALUE)) return 0;

   DWORD flags;
   if (GetHandleInformation(Handle, &flags)) return 1;
   else return 0;
}

//********************************************************************************************************************

ERR winAnalysePath(CSTRING Path, bool &IsDirectory, bool &IsSymbolicLink)
{
   if (!Path) return ERR::NullArgs;

   IsDirectory = false;
   IsSymbolicLink = false;

   WIN32_FILE_ATTRIBUTE_DATA fileData;
   if (!GetFileAttributesEx(Path, GetFileExInfoStandard, &fileData)) {
      return ERR::FileNotFound; // Path doesn't exist or access denied
   }

   auto attrs = fileData.dwFileAttributes;

   if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
      IsDirectory = true;

      // Check for junction/symbolic link
      if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
         HANDLE hDir = CreateFile(Path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
         if (hDir != INVALID_HANDLE_VALUE) {
            BYTE reparseBuffer[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
            DWORD bytesReturned;
            if (DeviceIoControl(hDir, FSCTL_GET_REPARSE_POINT, nullptr, 0, reparseBuffer, sizeof(reparseBuffer), &bytesReturned, nullptr)) {
               auto reparseData = (REPARSE_GUID_DATA_BUFFER *)reparseBuffer;
               if (reparseData->ReparseTag IS IO_REPARSE_TAG_MOUNT_POINT) IsSymbolicLink = true; // Technically a junction
               else if (reparseData->ReparseTag IS IO_REPARSE_TAG_SYMLINK) IsSymbolicLink = true;
            }
            CloseHandle(hDir);
         }
      }
   }
   else { // Is a file
      // Check for file symbolic link
      if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
         HANDLE hFile = CreateFile(Path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
         if (hFile != INVALID_HANDLE_VALUE) {
            BYTE reparseBuffer[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
            DWORD bytesReturned;
            if (DeviceIoControl(hFile, FSCTL_GET_REPARSE_POINT, nullptr, 0, reparseBuffer, sizeof(reparseBuffer), &bytesReturned, nullptr)) {
               auto reparseData = (REPARSE_GUID_DATA_BUFFER *)reparseBuffer;
               if (reparseData->ReparseTag IS IO_REPARSE_TAG_SYMLINK) {
                  IsSymbolicLink = true;
               }
            }
            CloseHandle(hFile);
         }
      }
   }

   return ERR::Okay; // Success
}

//********************************************************************************************************************

extern "C" void winSetDllDirectory(LPCSTR Path)
{
   SetDllDirectoryA(Path);
}

//********************************************************************************************************************

extern "C" ERR winWatchFile(int Flags, CSTRING Path, APTR WatchBuffer, HANDLE *Handle, int *WinFlags)
{
   if ((!Path) or (!Path[0]) or (!Handle) or (!WinFlags) or (!WatchBuffer)) return ERR::Args;

   *Handle = nullptr;
   *WinFlags = 0;

   bool is_folder, is_symlink;
   if (winAnalysePath(Path, is_folder, is_symlink) != ERR::Okay) return ERR::FileNotFound;

   int nflags = 0;
   if (Flags & MFF_READ) nflags |= FILE_NOTIFY_CHANGE_LAST_ACCESS;
   if (Flags & MFF_MODIFY) nflags |= FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
   if (Flags & MFF_CREATE) nflags |= FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
   if (Flags & MFF_DELETE) nflags |= FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
   if (Flags & MFF_OPENED) nflags |= FILE_NOTIFY_CHANGE_LAST_ACCESS;
   if (Flags & MFF_ATTRIB) nflags |= FILE_NOTIFY_CHANGE_SECURITY | FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_ATTRIBUTES;
   //if (Flags & MFF_CLOSED) nflags |= ?; // Not supported by Windows
   if (Flags & (MFF_MOVED|MFF_RENAME)) nflags |= FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;

   if (!nflags) return ERR::NoSupport;

   std::string monitor_path, resolved_path;

   if (not is_folder) { // For individual files, monitor the parent directory
      resolved_path = Path;
      size_t last_slash = resolved_path.find_last_of('\\');
      if (last_slash IS std::string::npos) return ERR::Args; // Invalid file path
      resolved_path = resolved_path.substr(0, last_slash);
   }
   else resolved_path = Path;

   monitor_path.assign(resolved_path);
   if (monitor_path.ends_with('\\')) monitor_path.pop_back();

   DWORD open_flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED;
   if (is_symlink) {
      // For reparse points, we may need to follow the link
      // depending on whether we want to monitor the link itself or the target
      open_flags |= FILE_FLAG_OPEN_REPARSE_POINT;
   }

   *Handle = CreateFile(monitor_path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, open_flags, nullptr);

   if (*Handle != INVALID_HANDLE_VALUE) {
      // Setup asynchronous monitor
      memset(WatchBuffer, 0, sizeof(OVERLAPPED));
      auto ovlap = (OVERLAPPED *)WatchBuffer;
      auto fni = (FILE_NOTIFY_INFORMATION *)(ovlap + 1);

      const DWORD buffer_size = sizeof(FILE_NOTIFY_INFORMATION) + (MAX_PATH * sizeof(WCHAR)) + sizeof(DWORD);

      BOOL watch_folders = (is_folder and (Flags & MFF_DEEP)) ? TRUE : FALSE;

      DWORD empty;
      if (not ReadDirectoryChangesW(*Handle, fni, buffer_size, watch_folders, nflags, &empty, ovlap, nullptr)) {
         auto error = GetLastError();
         CloseHandle(*Handle);
         *Handle = nullptr;
         return (error IS ERROR_ACCESS_DENIED) ? ERR::NoPermission : ERR::SystemCall;
      }

      *WinFlags = nflags;
      return ERR::Okay;
   }
   else {
      *Handle = nullptr;

      switch (GetLastError()) {
         case ERROR_FILE_NOT_FOUND: return ERR::FileNotFound;
         case ERROR_PATH_NOT_FOUND: return ERR::FileNotFound;
         case ERROR_ACCESS_DENIED:  return ERR::NoPermission;
         default: return ERR::SystemCall;
      }
   }
}

//********************************************************************************************************************

extern "C" ERR winReadChanges(HANDLE Handle, APTR WatchBuffer, int NotifyFlags, char *PathOutput, int PathSize, int *Status)
{
   DWORD bytes_out = 0;
   auto ovlap = (OVERLAPPED *)WatchBuffer;
   auto fni = (FILE_NOTIFY_INFORMATION *)(ovlap + 1);

   *Status = 0;
   PathOutput[0] = '\0';

   if (!GetOverlappedResult(Handle, ovlap, &bytes_out, false)) {
      DWORD error = GetLastError();
      if (error IS ERROR_IO_INCOMPLETE or error IS ERROR_IO_PENDING) {
         return ERR::NothingDone;
      }
      else return ERR::SystemCall;
   }

   // Validate we received enough data for at least the header
   if (bytes_out < sizeof(FILE_NOTIFY_INFORMATION)) return ERR::NothingDone;

   // Buffer corruption detection - validate the FILE_NOTIFY_INFORMATION structure
   if (!fni->Action or fni->FileNameLength > (MAX_PATH * sizeof(WCHAR))) {
      // Clear the buffer and re-subscribe to recover from corruption
      memset(fni, 0, bytes_out);
      memset(WatchBuffer, 0, sizeof(OVERLAPPED));

      DWORD empty;
      const DWORD buffer_size = sizeof(FILE_NOTIFY_INFORMATION) + (MAX_PATH * sizeof(WCHAR)) + sizeof(DWORD);
      ReadDirectoryChangesW(Handle, fni, buffer_size, true, NotifyFlags, &empty, ovlap, nullptr);
      return ERR::NothingDone;
   }

   // Validate buffer bounds before accessing filename
   DWORD required_size = sizeof(FILE_NOTIFY_INFORMATION) + fni->FileNameLength;
   if (required_size > bytes_out) {
      return ERR::BufferOverflow;
   }

   // Process the first notification in the buffer
   if (fni->FileNameLength > 0) {
      DWORD filename_length_chars = fni->FileNameLength / sizeof(WCHAR);

      // Convert Unicode filename to UTF-8 with proper error handling
      int result = WideCharToMultiByte(CP_UTF8, 0, fni->FileName, filename_length_chars, PathOutput, PathSize - 1, nullptr, nullptr);
      if (result <= 0 or result >= PathSize) {
         // Conversion failed or output too large
         PathOutput[0] = 0;
         return ERR::StringFormat;
      }
      PathOutput[result] = 0;  // Null terminate
   }

   // Map Windows actions to MFF flags more accurately
   switch (fni->Action) {
      case FILE_ACTION_ADDED:             *Status = MFF_CREATE; break;
      case FILE_ACTION_REMOVED:           *Status = MFF_DELETE; break;
      case FILE_ACTION_MODIFIED:          *Status = MFF_MODIFY; break;
      case FILE_ACTION_RENAMED_OLD_NAME:  *Status = MFF_MOVED; break;
      case FILE_ACTION_RENAMED_NEW_NAME:  *Status = MFF_MOVED; break;
      default: *Status = 0; break;
   }

   // Clear the processed notification to prevent reprocessing
   DWORD action = fni->Action;
   fni->Action = 0;

   // Handle multiple notifications in buffer if present
   if (fni->NextEntryOffset > 0 and fni->NextEntryOffset < bytes_out) {
      // Move remaining notifications to the beginning of the buffer
      DWORD remaining_size = bytes_out - fni->NextEntryOffset;
      if (remaining_size > 0 and remaining_size < bytes_out) {
         memmove(fni, (BYTE *)fni + fni->NextEntryOffset, remaining_size);
         memset((BYTE *)fni + remaining_size, 0, bytes_out - remaining_size); // Zero out the rest of the buffer
      }
   }
   else { // No more notifications, clear the buffer and re-subscribe
      memset(fni, 0, bytes_out);
      memset(WatchBuffer, 0, sizeof(OVERLAPPED));

      DWORD empty;
      const DWORD buffer_size = sizeof(FILE_NOTIFY_INFORMATION) + (MAX_PATH * sizeof(WCHAR)) + sizeof(DWORD);
      if (!ReadDirectoryChangesW(Handle, fni, buffer_size, true, NotifyFlags, &empty, ovlap, nullptr)) {
         return ERR::SystemCall;
      }
   }

   return (action != 0) ? ERR::Okay : ERR::NothingDone;
}

//********************************************************************************************************************

extern "C" int winSetFileTime(STRING Location, bool Folder, int16_t Year, int16_t Month, int16_t Day, int16_t Hour, int16_t Minute, int16_t Second)
{
   SYSTEMTIME time;
   FILETIME filetime, localtime;

   int flags = FILE_ATTRIBUTE_NORMAL, rw;
   if (Folder) {
      rw = FILE_SHARE_WRITE;
      flags |= FILE_FLAG_BACKUP_SEMANTICS;
   }
   else rw = FILE_SHARE_READ|FILE_SHARE_WRITE;

   if (auto handle = CreateFile(Location, GENERIC_WRITE, rw, nullptr,
         OPEN_EXISTING, flags, nullptr); handle != INVALID_HANDLE_VALUE) {
      time.wYear         = Year;
      time.wMonth        = Month;
      time.wDayOfWeek    = 0;
      time.wDay          = Day;
      time.wHour         = Hour;
      time.wMinute       = Minute;
      time.wSecond       = Second;
      time.wMilliseconds = 0;

      SystemTimeToFileTime(&time, &localtime);
      LocalFileTimeToFileTime(&localtime, &filetime);

      int err = SetFileTime(handle, &filetime, &filetime, &filetime);
      CloseHandle(handle);
      if (err) return 1;
   }

   return 0;
}

//********************************************************************************************************************

extern "C" void winFindClose(HANDLE Handle)
{
   FindClose(Handle);
}

//********************************************************************************************************************

extern "C" int winReadKey(LPCSTR Key, LPCSTR Value, LPBYTE Buffer, int Length)
{
   HKEY handle;
   int err = 0;
   DWORD length = Length;
   if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, Key, 0, KEY_READ, &handle) IS ERROR_SUCCESS) {
      if (RegQueryValueEx(handle, Value, 0, 0, Buffer, &length) IS ERROR_SUCCESS) {
         err = length-1;
      }
      CloseHandle(handle);
   }
   return err;
}

//********************************************************************************************************************

extern "C" int winReadRootKey(LPCSTR Key, LPCSTR Value, LPBYTE Buffer, int Length)
{
   HKEY handle;
   int err = 0;
   DWORD length = Length;
   if (RegOpenKeyEx(HKEY_CLASSES_ROOT, Key, 0, KEY_READ, &handle) IS ERROR_SUCCESS) {
      if (RegQueryValueEx(handle, Value, 0, 0, Buffer, &length) IS ERROR_SUCCESS) {
         err = length-1;
      }
      CloseHandle(handle);
   }
   return err;
}

//********************************************************************************************************************

extern "C" int winGetUserName(STRING Buffer, int Length)
{
   if (!Buffer or Length <= 0) return 0;
   if (Length > MAX_USERNAME) Length = MAX_USERNAME;

   DWORD len = Length;
   auto result = GetUserName(Buffer, &len);
   if (result and (int(len) < Length)) Buffer[len] = 0;
   return result ? len : 0;
}

//********************************************************************************************************************

extern "C" int winGetUserFolder(STRING Buffer, int Size)
{
   LPITEMIDLIST list;
   char path[MAX_PATH];
   int i = 0;
   if (SHGetSpecialFolderLocation(nullptr, CSIDL_APPDATA, &list) IS NOERROR) {
      if (SHGetPathFromIDList(list, path)) {
         for (i=0; (i < Size-1) and (path[i]); i++) Buffer[i] = path[i];
         if (Buffer[i-1] != '\\') Buffer[i++] = '\\';
         Buffer[i] = 0;
      }

      LPMALLOC malloc;
      if (SHGetMalloc(&malloc) IS NOERROR) { // An awkward (but necessary) method for freeing folder string
         malloc->Free(list);
         malloc->Release();
      }
   }
   return i;
}

//********************************************************************************************************************

extern "C" int winMoveFile(STRING oldname, STRING newname)
{
   return MoveFile(oldname, newname);
}

//********************************************************************************************************************

extern "C" int winSetEOF(CSTRING Location, __int64 Size)
{
   HANDLE handle;
   LARGE_INTEGER li;

   li.QuadPart = Size;
   if ((handle = CreateFile(Location, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) != INVALID_HANDLE_VALUE) {
      li.LowPart = SetFilePointer(handle, li.LowPart, &li.HighPart, FILE_BEGIN);

      if (li.LowPart IS INVALID_SET_FILE_POINTER and GetLastError() != NO_ERROR) {
         printerror();
      }
      else if (SetEndOfFile(handle)) {
         CloseHandle(handle);
         return 1;
      }
      else printerror();

      CloseHandle(handle);
   }
   else printerror();
   return 0;
}

//********************************************************************************************************************

extern "C" int winGetLogicalDrives(void)
{
   return GetLogicalDrives();
}

//********************************************************************************************************************

extern "C" int winGetLogicalDriveStrings(STRING Buffer, int Length)
{
   return GetLogicalDriveStrings(Length, Buffer);
}

//********************************************************************************************************************

extern ERR winGetVolumeInformation(STRING Volume, std::string &Label, std::string &FileSystem, int &Type)
{
   char label_buffer[80] = { 0 };
   char fs_buffer[32] = { 0 };

   switch (GetDriveType(Volume)) {
      case DRIVE_CDROM:     Type = DRIVETYPE_CDROM; break;
      case DRIVE_FIXED:     Type = DRIVETYPE_FIXED; break;
      case DRIVE_REMOVABLE: Type = DRIVETYPE_REMOVABLE; break;
      case DRIVE_REMOTE:    Type = DRIVETYPE_NETWORK; break;
      default: Type = 0;
   }

   if (GetVolumeInformation(Volume, label_buffer, sizeof(label_buffer), nullptr, nullptr, nullptr, fs_buffer, sizeof(fs_buffer))) {
      if (label_buffer[0]) Label.assign(label_buffer);
      if (fs_buffer[0]) FileSystem.assign(fs_buffer);
   }

   if (Type IS DRIVETYPE_REMOVABLE) {
      char drive[] = "\\\\?\\X:";
      drive[4] = Volume[0];
      if (auto hDevice = CreateFile(drive, 0, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr); hDevice != INVALID_HANDLE_VALUE) {
         char buffer[1024];
         DWORD dwOutBytes;
         STORAGE_PROPERTY_QUERY query = { .PropertyId = StorageDeviceProperty, .QueryType = PropertyStandardQuery };
         if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer, sizeof(buffer), &dwOutBytes, (LPOVERLAPPED)nullptr)) {
            auto info = (PSTORAGE_DEVICE_DESCRIPTOR)buffer;
            if (info->BusType IS BusTypeUsb) Type = DRIVETYPE_USB;
         }
         CloseHandle(hDevice);
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

extern "C" int winTestLocation(STRING Location, int8_t CaseSensitive)
{
   int len, result;
   HANDLE handle;
   WIN32_FIND_DATA find;
   char save;
   int i, savepos;

   for (len=0; Location[len]; len++);
   if ((Location[len-1] IS '/') or (Location[len-1] IS '\\')) {

      if (len IS 3) {
         // Checking for the existence of a drive letter - does not necessarily mean that there is media in the device.

         char volname[60], fsname[40];
         DWORD volserial, maxcomp, fileflags;

         if (GetVolumeInformation(Location, volname, sizeof(volname), &volserial, &maxcomp, &fileflags, fsname, sizeof(fsname))) {
            return LOC_DIRECTORY;
         }
         else return 0;
      }
      else {
         // We have been asked to check for the explicit existence of a folder.

         result = 0;
         savepos = len-1;
         save = Location[savepos];
         Location[savepos] = 0; // Remove the trailing slash
         if ((handle = FindFirstFile(Location, &find)) != INVALID_HANDLE_VALUE) {
            if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) result = LOC_DIRECTORY;
            else while (FindNextFile(handle, &find)) {
               if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                  result = LOC_DIRECTORY;
                  break;
               }
            }
            FindClose(handle);
         }

         if (CaseSensitive) {
            // Check that the filename of the given location matches that of the actual name set on the file system.

            len--;
            while ((len > 0) and (Location[len-1] != '/') and (Location[len-1] != '\\')) len--;
            for (i=0; (Location[len+i] IS find.cFileName[i]) and (find.cFileName[i]) and (Location[len+i]); i++);
            if ((!Location[len+i]) and (!find.cFileName[i])) return result; // Match
            else return 0; // Not a case sensitive match
         }

         Location[savepos] = save;
      }

      return result;
   }
   else if ((handle = FindFirstFile(Location, &find)) != INVALID_HANDLE_VALUE) {
      if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
         result = LOC_DIRECTORY;
      }
      else result = LOC_FILE;

      FindClose(handle);

      if (CaseSensitive) {
         // Check that the filename of the given location matches that of the actual name set on the file system.

         while ((len > 0) and (Location[len-1] != '/') and (Location[len-1] != '\\')) len--;
         for (i=0; (Location[len+i] IS find.cFileName[i]) and (find.cFileName[i]) and (Location[len+i]); i++);
         if ((!Location[len+i]) and (!find.cFileName[i])) return result; /* Match */
         else return 0; /* Not a case sensitive match */
      }

      return result;
   }
   else return 0;
}

//********************************************************************************************************************
// Helper function to remove read-only attribute and delete a file

static ERR delete_file_helper(const std::string &FilePath)
{
   DWORD attrib = GetFileAttributes(FilePath.c_str());
   if (attrib != INVALID_FILE_ATTRIBUTES) {
      if (attrib & FILE_ATTRIBUTE_READONLY) {
         attrib &= ~FILE_ATTRIBUTE_READONLY;
         SetFileAttributes(FilePath.c_str(), attrib);
      }
   }

   if (unlink(FilePath.c_str()) IS 0) return ERR::Okay;
   else {
      #ifdef __CYGWIN__
      return convert_errno(*__errno(), ERR::SystemCall);
      #else
      return convert_errno(errno, ERR::SystemCall);
      #endif
   }
}

//********************************************************************************************************************
// Helper function to remove directory after clearing read-only attribute

static ERR delete_directory_helper(const std::string &DirPath)
{
   auto attrib = GetFileAttributes(DirPath.c_str());
   if (attrib != INVALID_FILE_ATTRIBUTES) {
      if (attrib & FILE_ATTRIBUTE_READONLY) {
         attrib &= ~FILE_ATTRIBUTE_READONLY;
         SetFileAttributes(DirPath.c_str(), attrib);
      }
   }

   if (RemoveDirectory(DirPath.c_str())) return ERR::Okay;
   else return ERR::SystemCall;
}

//********************************************************************************************************************
// The Path must not include a trailing slash.

extern ERR delete_tree(std::string &Path, FUNCTION *Callback, struct FileFeedback *Feedback)
{
   if ((Callback) and (Feedback)) {
      Feedback->Path = Path.data();
      auto result = CALL_FEEDBACK(Callback, Feedback);
      if (result IS FFR::ABORT) return ERR::Cancelled;
      else if (result IS FFR::SKIP) return ERR::Okay;
   }

   WIN32_FIND_DATA find;
   auto path_size = Path.size();
   Path.append("\\*");
   auto handle = FindFirstFile(Path.c_str(), &find);
   Path.pop_back();

   if (handle != INVALID_HANDLE_VALUE) {
      int cont = 1;
      while (cont) {
         if ((find.cFileName[0] IS '.') and (find.cFileName[1] IS 0));
         else if ((find.cFileName[0] IS '.') and (find.cFileName[1] IS '.') and (find.cFileName[2] IS 0));
         else {
            Path.resize(path_size + 1);
            Path.append(find.cFileName);

            if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
               ERR result = delete_tree(Path, Callback, Feedback);
               if (result != ERR::Okay and result != ERR::Cancelled) {
                  // Continue with other files even if one fails
               }
            }
            else delete_file_helper(Path);
         }

         cont = FindNextFile(handle, &find);
      }

      FindClose(handle);
   }

   Path.resize(path_size);

   // Remove the file/folder itself using helper functions

   auto attrib = GetFileAttributes(Path.c_str());
   if (attrib != INVALID_FILE_ATTRIBUTES) {
      if (attrib & FILE_ATTRIBUTE_DIRECTORY) return delete_directory_helper(Path);
      else return delete_file_helper(Path);
   }
   else return ERR::FileNotFound;
}

//********************************************************************************************************************

extern "C" HANDLE winFindDirectory(STRING Location, HANDLE *Handle, STRING Result)
{
   WIN32_FIND_DATA find;
   int i;

   if (*Handle) {
      while (FindNextFile(*Handle, &find)) {
         if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            for (i=0; find.cFileName[i]; i++) Result[i] = find.cFileName[i];
            Result[i] = 0;
            return *Handle;
         }
      }

      FindClose(*Handle);
   }
   else if ((*Handle = FindFirstFile(Location, &find)) != INVALID_HANDLE_VALUE) {
      do {
         if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            for (i=0; find.cFileName[i]; i++) Result[i] = find.cFileName[i];
            Result[i] = 0;
            return *Handle;
         }
      } while (FindNextFile(*Handle, &find));

      FindClose(*Handle);
   }

   *Handle = nullptr;
   return nullptr;
}

//********************************************************************************************************************

extern "C" HANDLE winFindFile(CSTRING Location, HANDLE *Handle, STRING Result)
{
   WIN32_FIND_DATA find;
   int i;

   if (*Handle) {
      while (FindNextFile(*Handle, &find)) {
         if (!(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            for (i=0; find.cFileName[i]; i++) Result[i] = find.cFileName[i];
            Result[i] = 0;
            return *Handle;
         }
      }
      FindClose(*Handle);
   }
   else if ((*Handle = FindFirstFile(Location, &find)) != INVALID_HANDLE_VALUE) {
      do {
         if (!(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            for (i=0; find.cFileName[i]; i++) Result[i] = find.cFileName[i];
            Result[i] = 0;
            return *Handle;
         }
      } while (FindNextFile(*Handle, &find));

      FindClose(*Handle);
   }

   *Handle = nullptr;
   return nullptr;
}

/*********************************************************************************************************************
** Used by fs_scandir()
*/

extern "C" int winScan(HANDLE *Handle, CSTRING Path, STRING Name, long long *Size, struct DateTime *CreateTime,
   struct DateTime *WriteTime, int8_t *Dir, int8_t *Hidden, int8_t *ReadOnly, int8_t *Archive)
{
   WIN32_FIND_DATA find;
   int i;

   while (true) {
      if (*Handle IS (HANDLE)-1) {
         *Handle = FindFirstFile(Path, &find);
         if (*Handle IS INVALID_HANDLE_VALUE) return 0;
      }
      else if (!FindNextFile(*Handle, &find)) return 0;

      if ((find.cFileName[0] IS '.') and (find.cFileName[1] IS 0)) continue;
      if ((find.cFileName[0] IS '.') and (find.cFileName[1] IS '.') and (find.cFileName[2] IS 0)) continue;

      if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
         *Dir = true;
         *Size = 0;
      }
      else {
         *Dir = false;
         *Size = (find.nFileSizeHigh * (MAXDWORD+1)) + find.nFileSizeLow;
      }

      if (find.dwFileAttributes & (FILE_ATTRIBUTE_SYSTEM|FILE_ATTRIBUTE_HIDDEN)) *Hidden = true;
      else *Hidden = false;

      if (find.dwFileAttributes & FILE_ATTRIBUTE_READONLY) *ReadOnly = true;
      else *ReadOnly = false;

      if (find.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE) *Archive = true;
      else *Archive = false;

      for (i=0; (find.cFileName[i]) and (i < 254); i++) Name[i] = find.cFileName[i];
      Name[i] = 0;

      if (CreateTime) convert_time(&find.ftCreationTime, CreateTime);
      if (WriteTime) convert_time(&find.ftLastWriteTime, WriteTime);
      //if (LastAccessTime) convert_time(&find.ftLastAccessTime, LastAccessTime); // Last access time is not dependable despite it being a feature...

      return 1;
   }

   return 0;
}

//********************************************************************************************************************

extern "C" int winSetAttrib(CSTRING Path, int Flags)
{
   auto attrib = GetFileAttributes(Path);
   if (attrib IS INVALID_FILE_ATTRIBUTES) return 1;

   if (Flags & PERMIT_HIDDEN) attrib |= FILE_ATTRIBUTE_HIDDEN;
   else attrib &= ~FILE_ATTRIBUTE_HIDDEN;

   if (Flags & PERMIT_ARCHIVE) attrib |= FILE_ATTRIBUTE_ARCHIVE;
   else attrib &= ~FILE_ATTRIBUTE_ARCHIVE;

   if (Flags & PERMIT_WRITE) attrib &= ~FILE_ATTRIBUTE_READONLY;
   else attrib |= FILE_ATTRIBUTE_READONLY;

   SetFileAttributes(Path, attrib);

   return 0;
}

//********************************************************************************************************************

extern "C" void winGetAttrib(CSTRING Path, int *Flags)
{
   *Flags = 0;

   auto attrib = GetFileAttributes(Path);
   if (attrib IS INVALID_FILE_ATTRIBUTES) return;

   if (attrib & FILE_ATTRIBUTE_HIDDEN)   *Flags |= PERMIT_HIDDEN;
   if (attrib & FILE_ATTRIBUTE_ARCHIVE)  *Flags |= PERMIT_ARCHIVE;
   if (attrib & FILE_ATTRIBUTE_OFFLINE)  *Flags |= PERMIT_OFFLINE;

   if (attrib & FILE_ATTRIBUTE_READONLY) *Flags |= PERMIT_READ;
   else *Flags |= PERMIT_READ|PERMIT_WRITE;
}

//********************************************************************************************************************

extern "C" int winFileInfo(CSTRING Path, size_t *Size, struct DateTime *Time, int8_t *Folder)
{
   if (!Path) return 0;

   int len;
   for (len=0; Path[len]; len++);
   if (len < 1) return 0;

   WIN32_FIND_DATA find;
   HANDLE handle;

   if ((Path[len-1] IS '/') or (Path[len-1] IS '\\')) {
      std::string short_path(Path, len-1);
      handle = FindFirstFile(short_path.c_str(), &find);
   }
   else handle = FindFirstFile(Path, &find);

   if (handle IS INVALID_HANDLE_VALUE) return 0;

   if (Folder) {
      if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) *Folder = true;
      else *Folder = false;
   }

   if (Size) *Size = (find.nFileSizeHigh * (MAXDWORD+1)) + find.nFileSizeLow;

   if (Time) {
      if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
         convert_time(&find.ftCreationTime, Time);
      }
      else convert_time(&find.ftLastWriteTime, Time);
   }

   FindClose(handle);

   return 1;
}

//********************************************************************************************************************
// Returns true if the folder exists.

extern "C" int winCheckDirectoryExists(CSTRING Path)
{
   int len;

   for (len=0; Path[len]; len++);
   if (len <= 3) return 1; // Return true if the path is a drive letter

   if ((Path[0] IS '\\') and (Path[1] IS '\\')) {
      // UNC handling.  Use the widechar version of FindFirstFile() because it is required for UNC paths.

      #define SIZE_WSTR 400
      wchar_t wstr[SIZE_WSTR] = { '\\', '\\', '?', '\\', 'U', 'N', 'C', '\\' };
      int i;
      if ((i = MultiByteToWideChar(CP_UTF8, 0, Path+2, -1, wstr+8, SIZE_WSTR-12)) > 0) {
         i += 7;

         if (wstr[i-1] != '\\') wstr[i++] = '\\';
         wstr[i++] = '*';
         wstr[i] = 0;

         HANDLE handle;
         WIN32_FIND_DATAW find;
         if ((handle = FindFirstFileW(wstr, &find)) != INVALID_HANDLE_VALUE) {
            do {
               if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                  FindClose(handle);
                  return 1;
               }
               //else printf("Path exists but is not a folder.\n");

            } while (FindNextFileW(handle, &find));

            FindClose(handle);
         }
         //else printerror();
      }

      return 0;
   }
   else {
      WIN32_FIND_DATA find;
      HANDLE handle;

      if ((Path[len-1] IS '/') or (Path[len-1] IS '\\')) {
         std::string short_path(Path, len-1);
         handle = FindFirstFileA(short_path.c_str(), &find);
      }
      else handle = FindFirstFileA(Path, &find);

      if (handle != INVALID_HANDLE_VALUE) {
         do {
            if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
               FindClose(handle);
               return 1;
            }
            //else fprintf(stderr, "Path exists but is not a folder.\n");

         } while (FindNextFileA(handle, &find));

         FindClose(handle);
      }
      //else printerror();

      return 0;
   }
}

//********************************************************************************************************************
// Set system time on Windows - requires administrator privileges

extern "C" int winSetSystemTime(int16_t Year, int16_t Month, int16_t Day, int16_t Hour, int16_t Minute, int16_t Second)
{
   SYSTEMTIME st;
   ZeroMemory(&st, sizeof(st));

   st.wYear         = Year;
   st.wMonth        = Month;
   st.wDay          = Day;
   st.wHour         = Hour;
   st.wMinute       = Minute;
   st.wSecond       = Second;
   st.wMilliseconds = 0;

   // Set the system time - requires SE_SYSTEMTIME_NAME privilege
   if (SetSystemTime(&st)) {
      return 1; // Success
   }
   else {
      // Common error codes:
      // ERROR_PRIVILEGE_NOT_HELD (1314) - Process lacks required privilege
      // ERROR_INVALID_PARAMETER (87) - Invalid time values
      return 0; // Failure
   }
}
