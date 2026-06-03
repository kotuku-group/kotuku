/*********************************************************************************************************************

The source code for Kōtuku is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

-CATEGORY-
Name: System
-END-

*********************************************************************************************************************/

#include <stdlib.h>

#ifdef __unix__
 #include <stdio.h>
 #include <unistd.h>
 #include <signal.h>
 #include <sys/resource.h>

 #ifndef __ANDROID__
  #include <sys/msg.h>
 #endif

 #include <sys/time.h>
 #ifdef __linux__
  #include <sys/sysinfo.h>
 #elif __APPLE__
  #include <sys/sysctl.h>
 #endif
 #include <fcntl.h>
 #include <time.h>
 #include <string.h> // Required for memset() on OS X

#elif _WIN32
 #include <string.h> // Required for memmove()
#endif

#if defined(__ARM_FEATURE_CRC32)
 #ifdef _MSC_VER
  #include <intrin.h>
 #else
  #include <arm_acle.h>
 #endif
 #define PF_HAS_HW_CRC32 1
#endif

#ifdef __ANDROID__
 #include <android/log.h>
#endif

#include <math.h>

#include "defs.h"

using namespace kt;

//********************************************************************************************************************

thread_local std::shared_ptr<ThreadRecord> tlThreadRecord;

//********************************************************************************************************************
// Remove the calling thread from the global thread registry.  Safe to call even if the thread was never registered.

void deregister_thread(void)
{
   auto tid = get_thread_id();
   tlThreadRecord.reset();
   std::lock_guard lock(glmThreadRegistry);
   glThreadRegistry.erase(int(tid));
}

//********************************************************************************************************************
// Return the ThreadRecord for the calling thread (pointer guaranteed).

std::shared_ptr<ThreadRecord> get_thread_record(void)
{
   if (not tlThreadRecord) get_thread_id();
   return tlThreadRecord;
}

/*********************************************************************************************************************

-FUNCTION-
AllocateID: Generates unique ID's for general purposes.

This function generates unique ID's that can be used in other Core functions.  A `Type` indicator is required and the
resulting number will be unique to that `Type` only.

ID allocations are permanent, so there is no need to free the allocated ID once it is no longer required.

-INPUT-
int(IDTYPE) Type: The type of ID that is required.

-RESULT-
int: A unique ID matching the requested type will be returned.  This function can return zero if the Type is unrecognised, or if an internal error occurred.

*********************************************************************************************************************/

int AllocateID(IDTYPE Type)
{
   kt::Log log(__FUNCTION__);

   if (Type IS IDTYPE::MESSAGE) {
      auto id = ++glMessageIDCount;
      log.function("MessageID: %d", id);
      return id;
   }
   else if (Type IS IDTYPE::GLOBAL) {
      return ++glGlobalIDCount;
   }
   else if (Type IS IDTYPE::FUNCTION) {
      return ++glFunctionID;
   }

   return 0;
}

/*********************************************************************************************************************

-FUNCTION-
CurrentTask: Returns the active Task object.

This function returns the @Task object of the active process.

If there is a legitimate circumstance where there is no current task (e.g. if this function is called during
Core initialisation) then the "system task" may be returned, which has ownership of Core resources.

-RESULT-
obj(Task): Returns a pointer to the current Task object or NULL if failure.

-TAGS-
api-owns-result, nullable-result, pure-query

*********************************************************************************************************************/

objTask * CurrentTask(void)
{
   return glCurrentTask;
}

/*********************************************************************************************************************

-FUNCTION-
GetErrorMsg: Translates error codes into human readable strings.
Category: Logging

The GetErrorMsg() function converts error codes into human readable strings.  If the `Error` is invalid, a string of
"Unknown error code" is returned.

-INPUT-
error Error: The error code to lookup.

-RESULT-
cstr: A human readable string for the error code is returned.  By default error codes are returned in English, however if a translation table exists for the user's own language, the string will be translated.

-TAGS-
api-owns-result, null-terminated-result, non-null-result, pure-query

*********************************************************************************************************************/

CSTRING GetErrorMsg(ERR Code)
{
   if ((int(Code) < glTotalMessages) and (int(Code) > 0)) {
      return glMessages[int(Code)];
   }
   else if (Code IS ERR::Okay) return "Operation successful.";
   else return "Unknown error code.";
}

/*********************************************************************************************************************

-FUNCTION-
GenCRC32: Generates 32-bit IEEE 802.3 CRC checksum values.

This function is used internally for the generation of 32-bit CRC checksums compatible with IEEE 802.3.  It is made
available to clients to generate CRC values over any length of buffer space.  This function may be called repeatedly
by feeding it CRC values in a cycle, making it ideal for processing streamed data.

Note that string hashes in Kotuku use CRC-32C, which is incompatible with this function.

-INPUT-
uint CRC: If streaming data to this function, this value must reflect the most recently returned CRC integer.  Otherwise set to zero.
ptr Data: The data to generate a CRC value for.
uint Length: The length of the `Data` buffer.

-RESULT-
uint: Returns the computed 32 bit CRC value for the given data.

-TAGS-
does-not-take-ownership, pure-query
-END-

*********************************************************************************************************************/

// Slice-by-8 lookup tables for maximum performance
// Tables 1-7 are computed at runtime for slice-by-8 algorithm

alignas(64) static constexpr uint32_t crc_table_0[256] = {
   0x00000000L, 0x77073096L, 0xee0e612cL, 0x990951baL, 0x076dc419L,
   0x706af48fL, 0xe963a535L, 0x9e6495a3L, 0x0edb8832L, 0x79dcb8a4L,
   0xe0d5e91eL, 0x97d2d988L, 0x09b64c2bL, 0x7eb17cbdL, 0xe7b82d07L,
   0x90bf1d91L, 0x1db71064L, 0x6ab020f2L, 0xf3b97148L, 0x84be41deL,
   0x1adad47dL, 0x6ddde4ebL, 0xf4d4b551L, 0x83d385c7L, 0x136c9856L,
   0x646ba8c0L, 0xfd62f97aL, 0x8a65c9ecL, 0x14015c4fL, 0x63066cd9L,
   0xfa0f3d63L, 0x8d080df5L, 0x3b6e20c8L, 0x4c69105eL, 0xd56041e4L,
   0xa2677172L, 0x3c03e4d1L, 0x4b04d447L, 0xd20d85fdL, 0xa50ab56bL,
   0x35b5a8faL, 0x42b2986cL, 0xdbbbc9d6L, 0xacbcf940L, 0x32d86ce3L,
   0x45df5c75L, 0xdcd60dcfL, 0xabd13d59L, 0x26d930acL, 0x51de003aL,
   0xc8d75180L, 0xbfd06116L, 0x21b4f4b5L, 0x56b3c423L, 0xcfba9599L,
   0xb8bda50fL, 0x2802b89eL, 0x5f058808L, 0xc60cd9b2L, 0xb10be924L,
   0x2f6f7c87L, 0x58684c11L, 0xc1611dabL, 0xb6662d3dL, 0x76dc4190L,
   0x01db7106L, 0x98d220bcL, 0xefd5102aL, 0x71b18589L, 0x06b6b51fL,
   0x9fbfe4a5L, 0xe8b8d433L, 0x7807c9a2L, 0x0f00f934L, 0x9609a88eL,
   0xe10e9818L, 0x7f6a0dbbL, 0x086d3d2dL, 0x91646c97L, 0xe6635c01L,
   0x6b6b51f4L, 0x1c6c6162L, 0x856530d8L, 0xf262004eL, 0x6c0695edL,
   0x1b01a57bL, 0x8208f4c1L, 0xf50fc457L, 0x65b0d9c6L, 0x12b7e950L,
   0x8bbeb8eaL, 0xfcb9887cL, 0x62dd1ddfL, 0x15da2d49L, 0x8cd37cf3L,
   0xfbd44c65L, 0x4db26158L, 0x3ab551ceL, 0xa3bc0074L, 0xd4bb30e2L,
   0x4adfa541L, 0x3dd895d7L, 0xa4d1c46dL, 0xd3d6f4fbL, 0x4369e96aL,
   0x346ed9fcL, 0xad678846L, 0xda60b8d0L, 0x44042d73L, 0x33031de5L,
   0xaa0a4c5fL, 0xdd0d7cc9L, 0x5005713cL, 0x270241aaL, 0xbe0b1010L,
   0xc90c2086L, 0x5768b525L, 0x206f85b3L, 0xb966d409L, 0xce61e49fL,
   0x5edef90eL, 0x29d9c998L, 0xb0d09822L, 0xc7d7a8b4L, 0x59b33d17L,
   0x2eb40d81L, 0xb7bd5c3bL, 0xc0ba6cadL, 0xedb88320L, 0x9abfb3b6L,
   0x03b6e20cL, 0x74b1d29aL, 0xead54739L, 0x9dd277afL, 0x04db2615L,
   0x73dc1683L, 0xe3630b12L, 0x94643b84L, 0x0d6d6a3eL, 0x7a6a5aa8L,
   0xe40ecf0bL, 0x9309ff9dL, 0x0a00ae27L, 0x7d079eb1L, 0xf00f9344L,
   0x8708a3d2L, 0x1e01f268L, 0x6906c2feL, 0xf762575dL, 0x806567cbL,
   0x196c3671L, 0x6e6b06e7L, 0xfed41b76L, 0x89d32be0L, 0x10da7a5aL,
   0x67dd4accL, 0xf9b9df6fL, 0x8ebeeff9L, 0x17b7be43L, 0x60b08ed5L,
   0xd6d6a3e8L, 0xa1d1937eL, 0x38d8c2c4L, 0x4fdff252L, 0xd1bb67f1L,
   0xa6bc5767L, 0x3fb506ddL, 0x48b2364bL, 0xd80d2bdaL, 0xaf0a1b4cL,
   0x36034af6L, 0x41047a60L, 0xdf60efc3L, 0xa867df55L, 0x316e8eefL,
   0x4669be79L, 0xcb61b38cL, 0xbc66831aL, 0x256fd2a0L, 0x5268e236L,
   0xcc0c7795L, 0xbb0b4703L, 0x220216b9L, 0x5505262fL, 0xc5ba3bbeL,
   0xb2bd0b28L, 0x2bb45a92L, 0x5cb36a04L, 0xc2d7ffa7L, 0xb5d0cf31L,
   0x2cd99e8bL, 0x5bdeae1dL, 0x9b64c2b0L, 0xec63f226L, 0x756aa39cL,
   0x026d930aL, 0x9c0906a9L, 0xeb0e363fL, 0x72076785L, 0x05005713L,
   0x95bf4a82L, 0xe2b87a14L, 0x7bb12baeL, 0x0cb61b38L, 0x92d28e9bL,
   0xe5d5be0dL, 0x7cdcefb7L, 0x0bdbdf21L, 0x86d3d2d4L, 0xf1d4e242L,
   0x68ddb3f8L, 0x1fda836eL, 0x81be16cdL, 0xf6b9265bL, 0x6fb077e1L,
   0x18b74777L, 0x88085ae6L, 0xff0f6a70L, 0x66063bcaL, 0x11010b5cL,
   0x8f659effL, 0xf862ae69L, 0x616bffd3L, 0x166ccf45L, 0xa00ae278L,
   0xd70dd2eeL, 0x4e048354L, 0x3903b3c2L, 0xa7672661L, 0xd06016f7L,
   0x4969474dL, 0x3e6e77dbL, 0xaed16a4aL, 0xd9d65adcL, 0x40df0b66L,
   0x37d83bf0L, 0xa9bcae53L, 0xdebb9ec5L, 0x47b2cf7fL, 0x30b5ffe9L,
   0xbdbdf21cL, 0xcabac28aL, 0x53b39330L, 0x24b4a3a6L, 0xbad03605L,
   0xcdd70693L, 0x54de5729L, 0x23d967bfL, 0xb3667a2eL, 0xc4614ab8L,
   0x5d681b02L, 0x2a6f2b94L, 0xb40bbe37L, 0xc30c8ea1L, 0x5a05df1bL,
   0x2d02ef8dL
};

alignas(64) static uint32_t crc_table[8][256];
static std::once_flag glCRCInit;

static void init_crc32_tables(void)
{
   std::call_once(glCRCInit, []() {
      // Copy table 0
      std::copy(std::begin(crc_table_0), std::end(crc_table_0), crc_table[0]);

      // Generate tables 1-7
      for (int i = 1; i < 8; i++) {
         for (int j = 0; j < 256; j++) {
            crc_table[i][j] = crc_table[i-1][j] >> 8 ^ crc_table[0][crc_table[i-1][j] & 0xff];
         }
      }
   });
}

static uint32_t gen_crc32_slice_by_8(uint32_t CRC, const uint8_t *Data, uint32_t Length)
{
   init_crc32_tables();

   while (Length >= 8) {
      uint32_t one;
      uint32_t two;
      memcpy(&one, Data, sizeof(one));
      memcpy(&two, Data + 4, sizeof(two));
      one ^= CRC;

      CRC = crc_table[7][(one      ) & 0xff] ^
            crc_table[6][(one >>  8) & 0xff] ^
            crc_table[5][(one >> 16) & 0xff] ^
            crc_table[4][(one >> 24) & 0xff] ^
            crc_table[3][(two      ) & 0xff] ^
            crc_table[2][(two >>  8) & 0xff] ^
            crc_table[1][(two >> 16) & 0xff] ^
            crc_table[0][(two >> 24) & 0xff];

      Data += 8;
      Length -= 8;
   }

   while (Length > 0) {
      CRC = crc_table[0][(CRC ^ *Data++) & 0xff] ^ (CRC >> 8);
      Length--;
   }

   return CRC;
}

#ifdef PF_HAS_HW_CRC32
static uint32_t gen_crc32_hardware(uint32_t CRC, const uint8_t *Data, uint32_t Length)
{
   while (Length >= sizeof(uint64_t)) {
      uint64_t chunk;
      memcpy(&chunk, Data, sizeof(chunk));
      CRC = __crc32d(CRC, chunk);
      Data += sizeof(chunk);
      Length -= sizeof(chunk);
   }

   while (Length >= sizeof(uint32_t)) {
      uint32_t chunk;
      memcpy(&chunk, Data, sizeof(chunk));
      CRC = __crc32w(CRC, chunk);
      Data += sizeof(chunk);
      Length -= sizeof(chunk);
   }

   while (Length >= sizeof(uint16_t)) {
      uint16_t chunk;
      memcpy(&chunk, Data, sizeof(chunk));
      CRC = __crc32h(CRC, chunk);
      Data += sizeof(chunk);
      Length -= sizeof(chunk);
   }

   while (Length > 0) {
      CRC = __crc32b(CRC, *Data++);
      Length--;
   }

   return CRC;
}
#endif

uint32_t GenCRC32(uint32_t CRC, APTR Data, uint32_t Length)
{
   if (not Data) return 0;

   auto data = (const uint8_t *)Data;
   CRC = ~CRC;

   #ifdef PF_HAS_HW_CRC32
      CRC = gen_crc32_hardware(CRC, data, Length);
   #else
      CRC = gen_crc32_slice_by_8(CRC, data, Length);
   #endif

   return ~CRC;
}

/*********************************************************************************************************************

-FUNCTION-
GetResource: Retrieves miscellaneous resource identifiers.

The GetResource() function is used to retrieve miscellaneous resource and state information from the system
core.  Refer to the Resource identifier for the full list of available resource codes and their meaning.

C++ developers should use the `GetResourcePtr()` macro if a resource identifier is known to return a pointer.

-INPUT-
int(RES) Resource: The ID of the resource that you want to obtain.

-RESULT-
large: Returns the value of the resource that you have requested.  If the resource ID is not known by the Core, `NULL` is returned.

-TAGS-
blocking
-END-

*********************************************************************************************************************/

int64_t GetResource(RES Resource)
{
#ifdef __linux__
   struct sysinfo sys;
#endif

   // Internal note: Choose GetResource() over GetSystemState() when a value is R/W

   switch(Resource) {
      case RES::PRIVILEGED:      return glPrivileged;
      case RES::LOG_LEVEL:       return glLogLevel;
      case RES::PROCESS_STATE:   return MAXINT(glTaskState);
      case RES::LOG_DEPTH:       return tlDepth;
      case RES::JNI_ENV:         return (MAXINT)glJNIEnv;
      case RES::THREAD_ID:       return int(get_thread_id());
      case RES::DISPLAY_DRIVER:  if (not glDisplayDriver.empty()) return (MAXINT)glDisplayDriver.c_str(); else return 0;
      case RES::MAIN_THREAD:     return tlMainThread ? true : false;
      case RES::MAIN_THREAD_ID:  return int(glMainThreadID);

      case RES::MEMORY_USAGE: {
         #ifdef __linux__
            struct rusage usage;
            if (not getrusage(RUSAGE_SELF, &usage)) {
               // Return the maximum resident set size in bytes.
               return (int64_t)usage.ru_maxrss * 1024LL; // Convert to bytes
            }
            else return -1; // Error retrieving resource usage
         #elif _WIN32
            return winGetProcessMemoryUsage(glProcessID);
         #else
            return -1;
         #endif
      }

#ifdef __linux__
      // NB: This value is not cached.  Although unlikely, it is feasible that the total amount of physical RAM could
      // change during runtime.

      case RES::TOTAL_MEMORY: {
         if (not sysinfo(&sys)) return (int64_t)sys.totalram * (int64_t)sys.mem_unit;
         else return -1;
      }

      case RES::FREE_MEMORY: {
   #if 0
         // Unfortunately sysinfo() does not report on cached ram, which can be significant
         if (not sysinfo(&sys)) return (int64_t)(sys.freeram + sys.bufferram) * (int64_t)sys.mem_unit; // Buffer RAM is considered as 'free'
   #else
         char str[2048];
         int result;
         int64_t freemem = 0;
         if (ReadFileToBuffer("/proc/meminfo", str, sizeof(str)-1, &result) IS ERR::Okay) {
            int i = 0;
            while (i < result) {
               if (startswith("Cached", str+i)) freemem += strtoll(str+i, nullptr, 0) * 1024LL;
               else if (startswith("Buffers", str+i)) freemem += strtoll(str+i, nullptr, 0) * 1024LL;
               else if (startswith("MemFree", str+i)) freemem += strtoll(str+i, nullptr, 0) * 1024LL;

               while ((i < result) and (str[i] != '\n')) i++;
               i++;
            }
         }

         return freemem;
   #endif
      }

      case RES::TOTAL_SHARED_MEMORY:
         if (not sysinfo(&sys)) return (int64_t)sys.sharedram * (int64_t)sys.mem_unit;
         else return -1;

      case RES::TOTAL_SWAP:
         if (not sysinfo(&sys)) return (int64_t)sys.totalswap * (int64_t)sys.mem_unit;
         else return -1;

      case RES::FREE_SWAP:
         if (not sysinfo(&sys)) return (int64_t)sys.freeswap * (int64_t)sys.mem_unit;
         else return -1;

      case RES::CPU_SPEED: {
         static int cpu_mhz = 0;

         if (cpu_mhz) return cpu_mhz;

         auto file = objFile::create { fl::Path("drive1:proc/cpuinfo"), fl::Flags(FL::READ|FL::BUFFER) };

         if (file.ok()) {
            std::string line;
            while (file->readLine(line) IS ERR::Okay) {
               if (startswith("cpu MHz", line)) {
                  if (auto value = strchr(line.c_str(), ':')) cpu_mhz = int(strtod(value + 1, nullptr));
               }
            }
         }

         return cpu_mhz;
      }
#elif __APPLE__
#warning TODO: Support for sysctlbyname()
#endif

      default: //log.warning("Unsupported resource ID %d.", Resource);
         break;
   }

   return 0;
}
/*********************************************************************************************************************

-FUNCTION-
GetSystemState: Returns miscellaneous data values from the Core.

The GetSystemState() function is used to retrieve miscellaneous resource and environment values, such as resource
paths, the Core's version number and the name of the host platform.

The state values in the structure are static and will not change during runtime.

-RESULT-
cstruct(*SystemState): A read-only !SystemState structure is returned.

-TAGS-
static-result, non-null-result

*********************************************************************************************************************/

const SystemState * GetSystemState(void)
{
   static bool initialised = false;
   static SystemState state;

   if (not initialised) {
      // Initialise constants that won't change
      initialised = true;

      state.ConsoleFD = glConsoleFD;

      state.ConsoleType = glConsoleType;

      #ifdef __ANDROID__
         state.Platform = "Android";
      #elif __unix__
         state.Platform = "Linux";
      #elif _WIN32
         state.Platform = "Windows";
      #elif __APPLE__
         state.Platform = "OSX";
      #else
         state.Platform = "Unknown";
      #endif

      #ifdef KOTUKU_RELEASE_BUILD
         state.ReleaseBuild = 1;
      #else
         state.ReleaseBuild = 0;
      #endif

      #ifdef KOTUKU_STATIC
         state.StaticBuild = 1;
      #else
         state.StaticBuild = 0;
      #endif

      extern char glIDL[];
      state.IDL = glIDL;

      state.OpenInfo = &glOpenInfo;
   }

   state.Stage = glSystemState;
   return &state;
}

/*********************************************************************************************************************

-FUNCTION-
PreciseTime: Returns the current system time, in microseconds.

This function returns the current 'system time' in microseconds (1 millionth of a second).  The value is monotonic
if the host platform allows it (typically expressed as the amount of time that has elapsed since the system was
switched on).  The benefit of monotonic time is that it is unaffected by changes to the system clock, such as daylight
savings adjustments or manual changes by the user.

-RESULT-
large: Returns the system time in microseconds.  Could return zero in the extremely unlikely event of an error.

-TAGS-
pure-query

*********************************************************************************************************************/

int64_t PreciseTime(void)
{
   auto now = std::chrono::steady_clock::now();
   auto duration = now.time_since_epoch();
   return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

/*********************************************************************************************************************

-FUNCTION-
RegisterFD: Registers a file descriptor for monitoring when the task is asleep.

This function will register a file descriptor that will be monitored for activity when the task is sleeping.  When
activity occurs on the descriptor, the callback referenced in `Routine` will be called.  The callback should read all
information from the descriptor, as the process will not be able to sleep if data is back-logged.

The file descriptor should be configured as non-blocking before registration.  Blocking descriptors may cause the
program to hang if not handled carefully.

File descriptors support read and write states simultaneously, and a callback routine can be applied to either state.
Set the `RFD::READ` flag to apply the `Routine` to the read callback and `RFD::WRITE` for the write callback.  If neither
flag is specified, `RFD::READ` is assumed.  A file descriptor may have up to one subscription per flag, for example a read
callback can be registered, followed by a write callback in a second call. Individual callbacks can be removed by
combining the read/write flags with `RFD::REMOVE`.

The capabilities of this function and FD handling in general is developed to suit the host platform. On POSIX
compliant systems, standard file descriptors are used.  In Microsoft Windows, object handles are used and blocking
restrictions are not imposed.

Call the `DeregisterFD()` macro to simplify unsubscribing once the file descriptor is no longer needed or is destroyed.

-INPUT-
hhandle FD: The file descriptor that is to be watched.
int(RFD) Flags: Set to at least one of `READ`, `WRITE`, `EXCEPT`, `REMOVE`.
fptr(void hhandle ptr) Routine: The routine that will read from the descriptor when data is detected on it.  The prototype is `void Routine(HOSTHANDLE FD, APTR Data)`.
ptr Data: User specific data pointer that will be passed to the `Routine`.  Separate data pointers apply to the read and write states of operation.

-ERRORS-
Okay: The `FD` was successfully registered.
Args: The `FD` was set to a value of `-1`.
NoSupport: The host platform does not support the provided `FD`.

-TAGS-
callback-held, does-not-take-ownership, non-blocking
-END-

*********************************************************************************************************************/

#ifdef _WIN32
ERR RegisterFD(HOSTHANDLE FD, RFD Flags, void (*Routine)(HOSTHANDLE, APTR), APTR Data)
#else
ERR RegisterFD(int FD, RFD Flags, void (*Routine)(HOSTHANDLE, APTR), APTR Data)
#endif
{
   kt::Log log(__FUNCTION__);

   // Note that FD's < -1 are permitted for the registering of functions marked with RFD::ALWAYS_CALL

#ifdef _WIN32
   if (FD IS (HOSTHANDLE)-1) return log.warning(ERR::Args);
   // Network sockets are managed by the Network module backend.
   if ((Flags & RFD::SOCKET) != RFD::NIL) return log.warning(ERR::NoSupport);
#else
   if (FD IS -1) return log.warning(ERR::Args);
#endif

   if (glFDProtected) { // Cache the request while glFDTable is in use.
      glRegisterFD.emplace_back(FD, Routine, Data, Flags);
      return ERR::Okay;
   }

   if ((Flags & RFD::REMOVE) != RFD::NIL) {
      if ((Flags & (RFD::READ|RFD::WRITE|RFD::EXCEPT|RFD::ALWAYS_CALL)) IS RFD::NIL) Flags |= RFD::READ|RFD::WRITE|RFD::EXCEPT|RFD::ALWAYS_CALL;

      for (auto it = glFDTable.begin(); it != glFDTable.end();) {
         if ((it->FD IS FD) and (((it->Flags & (RFD::READ|RFD::WRITE|RFD::EXCEPT|RFD::ALWAYS_CALL)) & Flags) != RFD::NIL)) {
            if ((Routine) and (it->Routine != Routine)) it++;
            else it = glFDTable.erase(it);
         }
         else it++;
      }
      return ERR::Okay;
   }

   if ((Flags & (RFD::READ|RFD::WRITE|RFD::EXCEPT|RFD::REMOVE|RFD::ALWAYS_CALL)) IS RFD::NIL) Flags |= RFD::READ;

   for (auto &fd : glFDTable) {
      if ((fd.FD IS FD) and ((Flags & (fd.Flags & (RFD::READ|RFD::WRITE|RFD::EXCEPT|RFD::ALWAYS_CALL))) != RFD::NIL)) {
         fd.Routine = Routine;
         fd.Flags   = Flags;
         fd.Data    = Data;
         return ERR::Okay;
      }
   }

   log.function("FD: %" PRId64 ", Routine: %p, Flags: $%.2x (New)", (MAXINT)FD, Routine, int(Flags));

#ifdef _WIN32
   // Nothing to do for Win32
#else
   if ((not Routine) and (FD > 0)) fcntl(FD, F_SETFL, fcntl(FD, F_GETFL) | O_NONBLOCK); // Ensure that the FD is non-blocking
#endif

   glFDTable.emplace_back(FD, Routine, Data, Flags);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
SetResourcePath: Redefines the location of a system resource path.

The SetResourcePath() function changes the default locations of the Core's resource paths.

To read a resource path, use the ~GetSystemState() function.

-INPUT-
int(RP) PathType: The ID of the resource path to set.
cpp(strview) Path: The new location to set for the resource path.

-ERRORS-
Okay:
NullArgs:
Args:

-TAGS-
copies-input, path-preserved

*********************************************************************************************************************/

ERR SetResourcePath(RP PathType, const std::string_view &Path)
{
   kt::Log log(__FUNCTION__);

   if (Path.empty()) return ERR::NullArgs;

   log.function("Type: %d, Path: %.*s", int(PathType), int(Path.size()), Path.data());

   switch(PathType) {
      case RP::ROOT_PATH:
         glRootPath = Path;
         if ((!glRootPath.empty()) and (glRootPath.back() != '/') and (glRootPath.back() != '\\')) {
            #ifdef _WIN32
               glRootPath.push_back('\\');
            #else
               glRootPath.push_back('/');
            #endif
         }
         return ERR::Okay;

      case RP::SYSTEM_PATH:
         glSystemPath = Path;
         if ((!glSystemPath.empty()) and (glSystemPath.back() != '/') and (glSystemPath.back() != '\\')) {
            #ifdef _WIN32
               glSystemPath.push_back('\\');
            #else
               glSystemPath.push_back('/');
            #endif
         }
         return ERR::Okay;

      case RP::MODULE_PATH: // An alternative path to the system modules.  This was introduced for Android, which holds the module binaries in the assets folders.
         glModulePath = Path;
         if ((!glModulePath.empty()) and (glModulePath.back() != '/') and (glModulePath.back() != '\\')) {
            #ifdef _WIN32
               glModulePath += '\\';
            #else
               glModulePath += '/';
            #endif
         }
         return ERR::Okay;

      default:
         return ERR::Args;
   }
}

/*********************************************************************************************************************

-FUNCTION-
SetResource: Sets miscellaneous resource identifiers.

The SetResource() function is used to manipulate miscellaneous system resources.  Currently the following resources
are supported:

<types lookup="RES" type="Resource">
<type name="ALLOC_MEM_LIMIT">Adjusts the memory limit imposed on ~AllocMemory().  The `Value` specifies the memory limit in bytes.</>
<type name="LOG_LEVEL">Adjusts the current debug level.  The `Value` must be between 0 and 9, where 1 is the lowest level of debug output (errors only) and 0 is off.</>
<type name="PRIVILEGED_USER">If the `Value` is set to 1, this resource option puts the process in privileged mode (typically this enables full administrator rights).  This feature will only work for Unix processes that are granted admin rights when launched.  Setting the Value to 0 reverts to the user's permission settings.  SetResource() will return an error code indicating the level of success.</>
<type name="LOG_DEPTH">Changes the branch depth of the log output (affects indenting).</>
</>

-INPUT-
int(RES) Resource: The ID of the resource to be set.
large Value:    The new value to set for the resource.

-RESULT-
large: Returns the previous value of the `Resource`.  If the `Resource` value is invalid, `NULL` is returned.
-END-

*********************************************************************************************************************/

int64_t SetResource(RES Resource, int64_t Value)
{
   kt::Log log(__FUNCTION__);

#ifdef __unix__
   static int16_t privileged = 0;
#endif

   int64_t oldvalue = 0;

   switch(Resource) {
      case RES::CONSOLE_FD: glConsoleFD = (HOSTHANDLE)(MAXINT)Value; break;

      case RES::LOG_LEVEL:
         if ((Value >= 0) and (Value <= 9)) glLogLevel = Value;
         break;

      case RES::LOG_DEPTH: tlDepth = Value; break;

      case RES::JNI_ENV: glJNIEnv = L64PTR(Value); break;

      case RES::PRIVILEGED_USER:
#ifdef __unix__
         log.trace("Privileged User: %s, Current UID: %d, Depth: %d", (Value) ? "TRUE" : "FALSE", geteuid(), privileged);

         if (glPrivileged) return int64_t(ERR::Okay); // In privileged mode, the user is always an admin

         if (Value) { // Enable admin privileges
            oldvalue = int64_t(ERR::Okay);
            if (not privileged) {
               if (glUID) {
                  if (glUID != glEUID) {
                     seteuid(glEUID);
                     privileged++;
                  }
                  else {
                     log.msg("Admin privileges not available.");
                     oldvalue = int64_t(ERR::NoPermission); // Admin privileges are not available
                  }
               }
               else privileged++;; // The user already has admin privileges
            }
            else privileged++;
         }
         else {
            // Disable admin privileges
            if (privileged > 0) {
               privileged--;
               if (not privileged) {
                  if (glUID != glEUID) seteuid(glUID);
               }
            }
         }
#else
         return int64_t(ERR::Okay);
#endif
         break;

      default:
         log.warning("Unrecognised resource ID: %d, Value: %" PRId64, int(Resource), (int64_t)Value);
   }

   return oldvalue;
}

/*********************************************************************************************************************

-FUNCTION-
SubscribeTimer: Subscribes an object or function to the timer service.

This function creates a new timer subscription that will be called at regular intervals for the calling object.

A callback function must be provided that follows this prototype: `ERR Function(OBJECTPTR Subscriber, INT64 Elapsed, INT64 CurrentTime)`

The `Elapsed` parameter is the total number of microseconds that have elapsed since the last call.  The `CurrentTime`
parameter is set to the ~PreciseTime() value just prior to the `Callback` being called.  The callback function
can return `ERR::Terminate` at any time to cancel the subscription.  All other error codes are ignored.  Tiri callbacks
should call `check(ERR::Terminate)` to perform the equivalent of this behaviour.

To change the interval, call ~UpdateTimer() with the new value.  To release a timer subscription, call
~UpdateTimer() with the resulting `Subscription` handle and an `Interval` of zero.

Timer management is provisioned by the ~ProcessMessages() function.  Failure to regularly process incoming
messages will lead to unreliable timer cycles.  It should be noted that the smaller the `Interval` that has been used,
the more imperative regular message checking becomes.  Prolonged processing inside a timer routine can also impact on
other timer subscriptions that are waiting to be processed.

-INPUT-
double Interval:   The total number of seconds to wait between timer calls.
ptr(func) Callback: A callback function is required that will be called on each time cycle.
&ptr Subscription: Optional.  The subscription will be assigned an identifier that is returned in this parameter.

-ERRORS-
Okay:
NullArgs:
Args:
InvalidState: The subscriber is marked for termination.
SystemLocked:

-TAGS-
creates-resource, callback-held, blocking

*********************************************************************************************************************/

ERR SubscribeTimer(double Interval, FUNCTION *Callback, APTR *Subscription)
{
   kt::Log log(__FUNCTION__);

   if ((not Interval) or (not Callback)) return log.warning(ERR::NullArgs);
   if (Interval < 0) return log.warning(ERR::Args);

   auto subscriber = tlContext.back().obj;
   if (subscriber->collecting()) return log.warning(ERR::InvalidState);

   if (Callback->Type IS CALL::SCRIPT) log.msg(VLF::BRANCH|VLF::FUNCTION|VLF::DETAIL, "Interval: %.3fs", Interval);
   else log.msg(VLF::BRANCH|VLF::FUNCTION|VLF::DETAIL, "Callback: %p, Interval: %.3fs", Callback->Routine, Interval);

   if (auto lock = std::unique_lock{glmTimer, 1000ms}) {
      auto usInterval = int64_t(Interval * 1000000.0); // Scale the interval to microseconds
      if (usInterval <= 40000) {
         // TODO: Rapid timers should be synchronised with other existing timers to limit the number of
         // interruptions that occur per second.
      }

      auto it = glTimers.emplace(glTimers.end());
      auto subscribed = PreciseTime();
      it->SubscriberID    = subscriber->UID;
      it->Interval        = usInterval;
      it->PendingInterval = 0;
      it->LastCall        = subscribed;
      it->NextCall        = subscribed + usInterval;
      it->Routine         = *Callback;
      it->Locked          = false;
      it->Cycle           = glTimerCycle - 1;

      if (subscriber->UID > 0) it->Subscriber = subscriber;
      else it->Subscriber = nullptr;

      // For resource tracking purposes it is important for us to keep a record of the subscription so that
      // we don't treat the object address as valid when it's been removed from the system.

      subscriber->setFlag(NF::TIMER_SUB); // TODO: Use pin() instead?
      if (Subscription) *Subscription = &*it;
      return ERR::Okay;
   }
   else return log.warning(ERR::SystemLocked);
}

/*********************************************************************************************************************

-FUNCTION-
UpdateTimer: Modify or remove a subscription created by SubscribeTimer().

This function complements ~SubscribeTimer().  It can change the interval for an existing timer subscription,
or remove it if the Interval is set to zero.

-INPUT-
ptr Subscription: The timer subscription to modify.
double Interval: The new interval for the timer (measured in seconds), or zero to remove.

-ERRORS-
Okay:
NullArgs:
AlreadyLocked:
SystemLocked:

-TAGS-
blocking

*********************************************************************************************************************/

ERR UpdateTimer(APTR Subscription, double Interval)
{
   kt::Log log(__FUNCTION__);

   if (not Subscription) return log.warning(ERR::NullArgs);

   if (auto lock = std::unique_lock{glmTimer, 1000ms}) {
      auto timer = (CoreTimer *)Subscription;
      log.msg(VLF::DETAIL|VLF::BRANCH|VLF::FUNCTION, "Subscription: %p, New Interval: %.4f, Current Interval: %.4f", Subscription, Interval, timer->Interval / 1000000.0);
      if (Interval < 0) {
         // Special mode:
         //   Doesn't upgrade the timer immediately unless the new interval < existing interval.
         //   Extending the interval will apply it on the following cycle.
         auto usInterval = -(int64_t(Interval * 1000000.0));
         if (usInterval <= timer->Interval) {
            timer->Interval = usInterval;
            timer->PendingInterval = 0;
            auto next_call = PreciseTime() + usInterval;
            if (next_call < timer->NextCall) timer->NextCall = next_call;
         }
         else timer->PendingInterval = usInterval;
         return ERR::Okay;
      }
      else if (Interval > 0) {
         auto usInterval = int64_t(Interval * 1000000.0);
         timer->Interval = usInterval;
         timer->NextCall = PreciseTime() + usInterval;
         return ERR::Okay;
      }
      else {
         if (timer->Locked) {
            // A timer can't be removed during its execution, but we can nullify the function entry
            // and ProcessMessages() will automatically terminate it on the next cycle.
            timer->Routine.Type = CALL::NIL;
            return log.warning(ERR::AlreadyLocked);
         }

         FUNCTION script_routine;
         if (timer->Routine.isScript()) script_routine = timer->Routine;

         for (auto it=glTimers.begin(); it != glTimers.end(); it++) {
            if (timer IS &(*it)) {
               glTimers.erase(it);
               break;
            }
         }

         lock.unlock();

         if (script_routine.isScript()) {
            ((objScript *)script_routine.Context)->derefProcedure(script_routine);
         }

         return ERR::Okay;
      }
   }
   else return log.warning(ERR::SystemLocked);
}

/*********************************************************************************************************************

-FUNCTION-
WaitTime: Waits for a specified amount of seconds.

This function waits for a period of time as specified by the `Seconds` parameter.  While waiting,
your process will continue to process incoming messages in order to prevent the process' message queue from
developing a back-log.

WaitTime() can return earlier than the indicated timeout if a message handler returns `ERR::Terminate`, or if a
`MSGID::QUIT` message is sent to the task's message queue.

For non-main threads, the sleep can be interrupted by another thread calling ~WakeThread().

NOTE: If the thread is in a stopping state, e.g. from ~WakeThread(), this function will return
immediately.

-INPUT-
double Seconds: The number of seconds to wait for.  Fractional values are supported for sub-second precision.

-ERRORS-
Okay:
Cancelled: The thread has been requested to stop and cannot pause.

-TAGS-
blocking

-END-

*********************************************************************************************************************/

ERR WaitTime(double Seconds)
{
   // Determine message processing mode (negative seconds disable message processing)
   bool process_msg = tlMainThread and (Seconds >= 0.0);

   if (Seconds < 0.0) Seconds = -Seconds;

   // Convert to microseconds with high precision
   auto total_microseconds = int64_t(Seconds * 1000000.0);

   if (total_microseconds <= 0) return ERR::Okay; // Nothing to wait for

   auto record = get_thread_record();

   {
      if (record->state.load(std::memory_order_acquire) IS TSTATE::STOPPING) return ERR::Cancelled;
      record->state.store(TSTATE::PAUSED, std::memory_order_release);
   }

   if (process_msg) {
      auto end_time = PreciseTime() + total_microseconds;
      int64_t current_time;

      do {
         current_time = PreciseTime();
         if (current_time >= end_time) break;

         // Convert remaining time to milliseconds for ProcessMessages
         auto remaining_ms = (end_time - current_time) / 1000LL;
         if (ProcessMessages(PMF::NIL, remaining_ms) IS ERR::Terminate) break;
      } while (true);
   }
   else {
      std::unique_lock lock(record->mutex);
      record->cv.wait_for(lock, std::chrono::microseconds(total_microseconds),
         [&] { return record->interrupted.load(std::memory_order_acquire); });
      record->interrupted.store(false, std::memory_order_release);
   }

   auto error = ERR::Okay;
   auto expected = TSTATE::PAUSED;
   if (not record->state.compare_exchange_strong(expected, TSTATE::RUNNING, std::memory_order_acq_rel)) {
      error = ERR::Cancelled; // State was changed to STOPPING by WakeThread()
   }

   return error;
}

/*********************************************************************************************************************

-FUNCTION-
WakeThread: Interrupt a sleeping thread.

Call WakeThread() to interrupt a thread that is blocked in ~WaitTime() or similar, where supported.  The target
thread will return from as soon as it is able to acquire its internal lock.  If the target thread is not currently
sleeping, a pending interrupt is recorded so that its next sleep cycle will return immediately.

If `Stop` is set to true then the target thread will be put into a stopping state.  This will cause all future
sleep attempts in the target thread to be cancelled.

-INPUT-
int Thread: The target thread's unique ID, as returned by `GetResource(RES::THREAD_ID)`.
int Stop: If `true`, the target thread will be put into a stopping state.

-ERRORS-
Okay: The thread was successfully interrupted.
Search: No thread with the given ID was found in the registry.

-TAGS-
blocking

-END-

*********************************************************************************************************************/

ERR WakeThread(int Thread, int Stop)
{
   std::shared_ptr<ThreadRecord> record;

   {
      std::lock_guard lock(glmThreadRegistry);
      auto it = glThreadRegistry.find(Thread);
      if (it IS glThreadRegistry.end()) return ERR::Search;
      record = it->second;
   }

   bool paused;
   {
      // Lock required to prevent missed wake-ups: the target thread could transition to PAUSED
      // and enter cv.wait() between our flag write and notify call.
      std::lock_guard lock(record->mutex);
      paused = (record->state.load(std::memory_order_acquire) IS TSTATE::PAUSED);
      record->interrupted.store(true, std::memory_order_release);
      if (Stop) record->state.store(TSTATE::STOPPING, std::memory_order_release);
   }

   if (paused) {
      record->cv.notify_one();
      cvObjects.notify_all();   // Wake threads blocked in LockObject()
      cvResources.notify_all(); // Wake threads blocked in AccessMemory()
   }
   return ERR::Okay;
}
