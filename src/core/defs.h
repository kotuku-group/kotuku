#pragma once

#ifndef PLATFORM_CONFIG_H
#include <kotuku/config.h>
#endif

#include <set>
#include <deque>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <condition_variable>
#include <chrono>
#include <array>
#include <atomic>
#include <thread>
#include <algorithm>
#include <optional>
#include <string_view>
#include <ankerl/unordered_dense.h>
#include <unordered_set>

using namespace std::chrono_literals;

#define PRV_CORE
#define PRV_CORE_MODULE
#ifndef __system__
#define __system__
#endif

#ifdef __unix__
 #include <fcntl.h>
 #include <sys/un.h>
 #include <sys/socket.h>
 #include <pthread.h>
 #include <semaphore.h>
#elif defined(_WIN32)
 #include <fcntl.h>
#endif

#include "microsoft/windefs.h"

// See the makefile for optional defines

constexpr int MAX_THREADS   = 20;  // Maximum number of threads per process.

#define CLASSDB_HEADER 0x7f887f89

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif

#ifdef _WIN32
#define WIN32OPEN O_BINARY
#else
#define WIN32OPEN 0
#endif

constexpr int DRIVETYPE_REMOVABLE = 1;
constexpr int DRIVETYPE_CDROM     = 2;
constexpr int DRIVETYPE_FIXED     = 3;
constexpr int DRIVETYPE_NETWORK   = 4;
constexpr int DRIVETYPE_USB       = 5;

#define DEFAULT_VIRTUALID 0xffffffff

#define CODE_MEMH 0x4D454D48L
#define CODE_MEMT 0x4D454D54L

#ifdef _WIN32
   typedef void * MODHANDLE;
   typedef void * THREADLOCK;
   typedef void * CONDLOCK;
#else
   typedef void * MODHANDLE;
   typedef pthread_mutex_t THREADLOCK;
   typedef pthread_cond_t CONDLOCK;
#endif

#define BREAKPOINT { uint8_t *nz = 0; nz[0] = 0; }

#include <kotuku/system/errors.h>
#include <kotuku/system/types.h>
#include <kotuku/system/registry.h>

#include <stdarg.h>
#include <stdio.h>

struct ChildEntry;
struct ObjectInfo;
struct MemInfo;
struct ListTasks;
struct DateTime;
struct RGB8;
struct pfBase64Decode;
struct FileInfo;
struct DirInfo;
struct ActionTable;
struct FileFeedback;
struct ResourceManager;
struct MsgHandler;

class objFile;
class objStorageDevice;
class objConfig;
class objMetaClass;
class objTask;

enum class RES    : int;
enum class RP     : int;
enum class IDTYPE : int;
enum class TSTATE : int8_t;
enum class LOC    : int;
enum class STT    : int;
enum class NF     : uint32_t;
enum class FOF    : uint32_t;
enum class RFD    : uint32_t;
enum class PMF    : uint32_t;
enum class MSF    : uint32_t;
enum class RDF    : uint32_t;
enum class RSF    : uint32_t;
enum class LDF    : uint32_t;
enum class VOLUME : uint32_t;
enum class STR    : uint32_t;
enum class SCF    : uint32_t;
enum class SBF    : uint32_t;
enum class SMF    : uint32_t;
enum class VLF    : uint32_t;
enum class MFF    : uint32_t;
enum class DEVICE : int64_t;
enum class PERMIT : uint32_t;
enum class CCF    : uint32_t;
enum class MEM    : uint32_t;
enum class ALF    : uint16_t;
enum class CONTYPE : int;
enum class EVG    : int;
enum class AC     : int;
enum class MSGID  : int;

constexpr int LC_LIMIT = 3;

struct LogCallback {
   void (*Callback)(CSTRING, CSTRING, int, int, int);
   int DepthLimit;
   int LogLimit;
   LogCallback() : Callback(nullptr), DepthLimit(-1), LogLimit(-1) {}
};

enum {
   LCS_EMPTY,
   LCS_WRITING,
   LCS_ACTIVE
};

struct LogCallbackSlot {
   LogCallback Callback;
   std::atomic_uint8_t State = LCS_EMPTY;
};

struct THREADID : strong_typedef<THREADID, int> { // Internal thread ID, unrelated to the host platform.
   // Make constructors available
   using strong_typedef::strong_typedef;
   bool operator==(const THREADID & other) const { return int(*this) == int(other); }
};

struct rkWatchPath {
   HOSTHANDLE Handle;    // The handle for the file being monitored, can be a special reference for virtual paths
   FUNCTION   Routine;   // Routine to call on event trigger
   MFF        Flags;     // Event mask (original flags supplied to Watch)
   uint32_t   VirtualID; // If monitored path is virtual, this refers to an ID in the glVirtual table

#ifdef _WIN32
   int WinFlags;
#endif
};

#include <kotuku/vector.hpp>

#include <kotuku/main.h>
#include <kotuku/strings.hpp>
#include <kotuku/system/internals.h>
#include "prototypes.h"

using namespace kt;

struct ThreadMessage {
   OBJECTID ThreadID;    // Internal
   FUNCTION Callback;
};

struct ThreadActionMessage {
   AC        ActionID;  // The action to execute.
   OBJECTID  ObjectID;  // ID of the target object (for queue dispatch).
   ERR       Error;     // The error code resulting from the action's execution.
   FUNCTION  Callback;  // Callback function to execute on action completion.
   bool      DispatchNext = true; // False for callbacks emitted while draining a queue.
};

// Queued async action, waiting for the same-object action to complete.

struct QueuedAction {
   OBJECTID  ObjectID;
   AC        ActionID;
   int       ArgsSize;
   std::vector<int8_t> Parameters;
   FUNCTION  Callback;
};

//********************************************************************************************************************

extern std::mutex glmPrint;               // For message logging only.

extern std::timed_mutex glmGeneric;       // A misc. internal mutex, strictly not recursive.
extern std::timed_mutex glmObjectLocking; // For LockObject() and ReleaseObject()
extern std::timed_mutex glmClassDB;       // For glClassDB
extern std::shared_timed_mutex glmFieldKeys; // For glFields
extern std::shared_timed_mutex glmVolumes;   // For glVolumes

extern std::recursive_timed_mutex glmTimer;        // For timer subscriptions.
extern std::shared_timed_mutex glmObjectLookup;    // For glObjectLookup

extern std::recursive_mutex glmMemory;
extern std::recursive_mutex glmMsgHandler;
extern std::recursive_mutex glmAsyncActions;

extern std::mutex glmActionQueue;
extern std::unordered_map<OBJECTID, std::deque<QueuedAction>> glActionQueues;
extern std::unordered_set<OBJECTID> glActiveAsyncObjects;
extern std::unordered_set<OBJECTID> glCancelledAsyncObjects;
extern std::unordered_map<OBJECTID, int> glAsyncObjectThreads;

extern std::condition_variable_any cvResources;
extern std::condition_variable_any cvObjects;

// Per-thread record for the global thread registry.  Threads are registered on first use of get_thread_id() and
// deregistered on thread destruction.  The condition variable allows other threads to interrupt a sleeping thread
// via WakeThread().

struct ThreadRecord {
   std::mutex mutex;                            // Guards cv.wait() and compound updates from WakeThread()
   std::condition_variable cv;
   std::atomic<TSTATE> state = TSTATE::RUNNING; // Readable without locking; writes from other threads require mutex
   std::atomic<bool> interrupted = false;        // Readable without locking; set by WakeThread() under mutex
};

extern std::mutex glmThreadRegistry;
extern std::unordered_map<int, std::shared_ptr<ThreadRecord>> glThreadRegistry;
extern thread_local std::shared_ptr<ThreadRecord> tlThreadRecord;

extern ActionMessage *glCurrentActionMsg;

//********************************************************************************************************************

inline std::string_view get_volume(std::string_view Path)
{
   if (auto pos = Path.find(':'); pos != std::string::npos) {
      return std::string_view(Path.data(), pos);
   }
   else return std::string_view();
}

//********************************************************************************************************************

struct CaseInsensitiveMap {
   using is_transparent = void;

   bool operator() (std::string_view Lhs, std::string_view Rhs) const noexcept {
      auto length = std::min(Lhs.size(), Rhs.size());
      for (size_t i=0; i < length; i++) {
         auto lhs = std::tolower((unsigned char)Lhs[i]);
         auto rhs = std::tolower((unsigned char)Rhs[i]);
         if (lhs < rhs) return true;
         if (rhs < lhs) return false;
      }

      return Lhs.size() < Rhs.size();
   }
};

struct CaseInsensitiveHash {
   using is_transparent = void;

   std::size_t operator()(std::string_view Value) const noexcept {
      std::size_t hash = 5381;
      for (auto ch : Value) {
         hash = ((hash << 5) + hash) + std::tolower(uint8_t(ch));
      }
      return hash;
   }
};

struct CaseInsensitiveEqual {
   using is_transparent = void;

   bool operator()(std::string_view Lhs, std::string_view Rhs) const noexcept {
      if (Lhs.size() != Rhs.size()) return false;

      for (size_t i=0; i < Lhs.size(); i++) {
         auto lhs = std::tolower(uint8_t(Lhs[i]));
         auto rhs = std::tolower(uint8_t(Rhs[i]));
         if (lhs != rhs) return false;
      }

      return true;
   }
};

using OBJECTLOOKUP = ankerl::unordered_dense::map<uint32_t, std::vector<Object *>>;

//********************************************************************************************************************

struct ActionSubscription {
   OBJECTPTR Subscriber; // The object that initiated the subscription
   OBJECTID SubscriberID; // Required for sanity checks on subscriber still existing.
   void (*Callback)(OBJECTPTR, ACTIONID, ERR, APTR, APTR);
   APTR Meta;

   ActionSubscription() : Subscriber(nullptr), SubscriberID(0), Callback(nullptr), Meta(nullptr) { }

   ActionSubscription(OBJECTPTR pContext, void (*pCallback)(OBJECTPTR, ACTIONID, ERR, APTR, APTR), APTR pMeta) :
      Subscriber(pContext), SubscriberID(pContext->UID), Callback(pCallback), Meta(pMeta) { }

   ActionSubscription(OBJECTPTR pContext, APTR pCallback, APTR pMeta) :
      Subscriber(pContext), SubscriberID(pContext->UID), Callback((void (*)(OBJECTPTR, ACTIONID, ERR, APTR, APTR))pCallback), Meta(pMeta) { }
};

struct virtual_drive {
   uint32_t VirtualID;   // Hash name of the volume, not including the trailing colon
   int  DriverSize;  // The driver may reserve a private area for its own structure attached to DirInfo.
   std::string Name;  // Volume name, including the trailing colon at the end
   bool CaseSensitive;
   ERR (*ScanDir)(DirInfo *);
   ERR (*Rename)(std::string_view, std::string_view);
   ERR (*Delete)(std::string_view, FUNCTION *);
   ERR (*OpenDir)(DirInfo *);
   ERR (*CloseDir)(DirInfo *);
   ERR (*Obsolete)(void);
   ERR (*TestPath)(std::string &, RSF, LOC *);
   ERR (*WatchPath)(class extFile *);
   void (*IgnoreFile)(class extFile *);
   ERR (*GetInfo)(std::string_view, FileInfo &);
   ERR (*GetDeviceInfo)(std::string_view, objStorageDevice *);
   ERR (*IdentifyFile)(std::string_view, CLASSID *, CLASSID *);
   ERR (*CreateFolder)(std::string_view, PERMIT);
   ERR (*SameFile)(std::string_view, std::string_view);
   ERR (*ReadLink)(std::string_view, STRING *);
   ERR (*CreateLink)(std::string_view, std::string_view);
   inline bool is_default() const { return VirtualID IS 0; }
   inline bool is_virtual() const { return VirtualID != 0; }
};

extern const virtual_drive glFSDefault;
extern std::mutex glmVirtual;
extern ankerl::unordered_dense::map<uint32_t, virtual_drive> glVirtual;

//********************************************************************************************************************
// Resource definitions.

#define MEMHEADER 12    // 8 bytes at start for MEMH and MemoryID, 4 at end for MEMT

// Turning off USE_SHM means that the shared memory pool is available to all processes by default.

#ifdef __ANDROID__
  #undef USE_SHM // Should be using ashmem
  #define SHMKEY 0x0009f830 // Keep the key value low as we will be incrementing it

  #ifdef USE_SHM
  #else
    extern int glMemoryFD;
  #endif
#elif __unix__
  #define USE_SHM TRUE
  #define SHMKEY 0x0009f830 // Keep the key value low as we will be incrementing it

  #ifdef USE_SHM
    #define MEMORYFILE           "/tmp/kotuku.mem"
  #else
    // To mount a 32MB RAMFS filesystem for this method:
    //
    //    mkdir -p /RAM1
    //    mount -t ramfs none /tmp/ramfs -o maxsize=32000

    #define MEMORYFILE           "/tmp/ramfs/kotuku.mem"

    extern int glMemoryFD;
  #endif
#endif

enum {
   RT_OBJECT,
   RT_SLEEP // Thread is sleeping in ProcessMessages / sleep_task
};

class extMetaClass : public objMetaClass {
   public:
   using create = kt::Create<extMetaClass>;
   class extMetaClass *Base;            // Reference to the base class if this is a derived class
   std::vector<Field> FieldLookup;      // Field dictionary for base-class fields
   std::vector<MethodEntry> Methods;    // Original method array supplied by the module.
   std::vector<extMetaClass *> SubClasses; // List of all associated derived classes
   const struct FieldArray *SubFields;  // Extra fields defined by the derived class
   class RootModule *Root;              // Root module that owns this class, if any.
   uint8_t Local[8];                    // Local object references (by field indexes), in order
   std::string Location;                // Location of the class binary, this field exists purely for caching the location string if the client reads it
   ActionEntry ActionTable[int(AC::END)];
   int16_t OriginalFieldTotal;
   uint16_t BaseCeiling;                   // FieldLookup ceiling value for the base-class fields

   extMetaClass() {
      Local[0] = 0xff;
   }
};

class extFile : public objFile {
   public:
   using create = kt::Create<extFile>;
   struct DateTime prvModified;
   struct DateTime prvCreated;
   std::string Path;
   std::string prvIcon;
   std::string prvLine;
   std::string prvResolvedPath;
   #ifdef __unix__
      std::string prvLink;
   #endif
   int64_t Size;
   int64_t ProgressTime;
   #ifdef _WIN32
      int  Stream;
   #else
      APTR  Stream;
   #endif
   struct rkWatchPath *prvWatch;
   OBJECTPTR ProgressDialog;
   struct DirInfo *prvList;
   PERMIT Permissions;
   bool   isFolder;
   int   Handle;         // Native system file handle

   extFile() {
      Handle = -1;
      Permissions = PERMIT::READ|PERMIT::WRITE|PERMIT::GROUP_READ|PERMIT::GROUP_WRITE;
   }
};

class extConfig : public objConfig {
   public:
   using create = kt::Create<extConfig>;
   uint32_t CRC;   // CRC32, for determining if config data has been altered
};

class extStorageDevice : public objStorageDevice {
   public:
   using create = kt::Create<extStorageDevice>;
   std::string DeviceID;   // Unique ID for the filesystem, if available
   std::string Volume;
};

class extThread : public objThread {
   public:
   using create = kt::Create<extThread>;

   std::jthread::native_handle_type Handle;
   std::jthread::id ThreadID;
   std::jthread *CPPThread;
   std::atomic_int InterruptThreadID = 0; // Internal thread ID used by WakeThread() for cooperative shutdown
   FUNCTION Routine;
   FUNCTION Callback;
   std::atomic_bool Active;
};

class extTask : public objTask {
   public:
   using create = kt::Create<extTask>;
   ankerl::unordered_dense::map<std::string, std::string, CaseInsensitiveHash, CaseInsensitiveEqual> Fields; // Variable field storage
   kt::vector<std::string> Parameters; // Arguments (string array)
   uint64_t AffinityMask;  // CPU affinity mask for process/thread binding
   MEMORYID MessageMID;
   bool     ReturnCodeSet;    // TRUE if the ReturnCode has been set
   bool     QuitCalled;       // TRUE if TASK_Quit has been called before
   FUNCTION ErrorCallback;
   FUNCTION OutputCallback;
   FUNCTION ExitCallback;
   FUNCTION InputCallback;
   MsgHandler *MsgAction;
   MsgHandler *MsgFree;
   MsgHandler *MsgDebug;
   MsgHandler *MsgWaitForObjects;
   MsgHandler *MsgQuit;
   MsgHandler *MsgEvent;
   MsgHandler *MsgThreadCallback;
   MsgHandler *MsgThreadAction;

   #ifdef __unix__
      int InFD = -1;       // stdin FD for receiving output from launched task
      int ErrFD = -1;      // stderr FD for receiving output from launched task
   #endif
   #ifdef _WIN32
      std::string Env;
      APTR Platform;
   #endif
   struct ActionEntry Actions[int(AC::END)]; // Action routines to be intercepted by the program

   extTask() {
      TimeOut = 60 * 60 * 24;
   }
};

//********************************************************************************************************************

struct TaskRecord {
   int64_t  CreationTime;  // Time at which the task slot was created
   int      ProcessID;     // Core process ID
   OBJECTID TaskID;        // Representative task object.
   int      ReturnCode;    // Return code
   bool     Returned;      // Process has finished (the ReturnCode is set)
   #ifdef _WIN32
      WINHANDLE Lock;      // The semaphore to signal when a message is sent to the task
   #endif

   TaskRecord(extTask *Task) {
      ProcessID    = Task->ProcessID;
      CreationTime = PreciseTime() / 1000LL;
      TaskID       = Task->UID;
      ReturnCode   = 0;
      Returned     = false;
   }
};

//********************************************************************************************************************

class extModule : public objModule {
   public:
   using create = kt::Create<extModule>;
   std::string Name;     // Name of the module
   APTR   prvMBMemory;   // Module base memory
};

//********************************************************************************************************************
// Class database.

struct extClassRecord : public ClassRecord {
   static constexpr int MIN_SIZE = sizeof(CLASSID) + sizeof(CLASSID) + sizeof(int) + (sizeof(int) * 5);

   extClassRecord() { }

   inline extClassRecord(extMetaClass *pClass, std::optional<std::string> pPath = std::nullopt) {
      ClassID  = pClass->ClassID;
      ParentID = (pClass->BaseClassID IS pClass->ClassID) ? CLASSID::NIL : pClass->BaseClassID;
      Category = pClass->Category;

      Name.assign(pClass->ClassName);

      if (pPath.has_value()) Path.assign(pPath.value());
      else if (not pClass->Path.empty()) Path.assign(pClass->Path);

      if (not pClass->FileExtension.empty()) Extension.assign(pClass->FileExtension);
      if (not pClass->FileHeader.empty()) Header.assign(pClass->FileHeader);
      if (not pClass->Icon.empty()) Icon.assign(pClass->Icon);
      if (not pClass->FileDescription.empty()) Description.assign(pClass->FileDescription);
   }

   inline extClassRecord(CLASSID pClassID, std::string pName, CSTRING pExtension = nullptr, CSTRING pHeader = nullptr, CSTRING pIcon = nullptr, CSTRING pDescription = nullptr) {
      ClassID  = pClassID;
      ParentID = CLASSID::NIL;
      Category = CCF::SYSTEM;
      Name     = pName;
      Path     = "modules:core";
      if (pExtension)   Extension   = pExtension;
      if (pHeader)      Header      = pHeader;
      if (pIcon)        Icon        = pIcon;
      if (pDescription) Description = pDescription;
   }

   inline ERR write(objFile *File) {
      if (File->write(&ClassID, sizeof(ClassID), nullptr) != ERR::Okay) return ERR::Write;
      if (File->write(&ParentID, sizeof(ParentID), nullptr) != ERR::Okay) return ERR::Write;
      if (File->write(&Category, sizeof(Category), nullptr) != ERR::Okay) return ERR::Write;

      auto size = int(Name.size());
      File->write(&size, sizeof(size));
      File->write(Name.c_str(), size);

      size = Path.size();
      File->write(&size, sizeof(size));
      if (size) File->write(Path.c_str(), size);

      size = Extension.size();
      File->write(&size, sizeof(size));
      if (size) File->write(Extension.c_str(), size);

      size = Header.size();
      File->write(&size, sizeof(size));
      if (size) File->write(Header.c_str(), size);

      size = Icon.size();
      File->write(&size, sizeof(size));
      if (size) File->write(Icon.c_str(), size);

      size = Description.size();
      File->write(&size, sizeof(size));
      if (size) File->write(Description.c_str(), size);
      return ERR::Okay;
   }

   inline ERR read(objFile *File) {
      if (File->read(&ClassID, sizeof(ClassID)) != ERR::Okay) return ERR::Read;
      if (File->read(&ParentID, sizeof(ParentID)) != ERR::Okay) return ERR::Read;
      if (File->read(&Category, sizeof(Category)) != ERR::Okay) return ERR::Read;

      char buffer[256];
      int size = 0;
      File->read(&size, sizeof(size));
      if (size < std::ssize(buffer)) {
         if (size > 0) {
            File->read(buffer, size);
            Name.assign(buffer, size);
         }
      }
      else return ERR::BufferOverflow;

      File->read(&size, sizeof(size));
      if (size < std::ssize(buffer)) {
         if (size > 0) {
            File->read(buffer, size);
            Path.assign(buffer, size);
         }
      }
      else return ERR::BufferOverflow;

      File->read(&size, sizeof(size));
      if (size < std::ssize(buffer)) {
         if (size > 0) {
            File->read(buffer, size);
            Extension.assign(buffer, size);
         }
      }
      else return ERR::BufferOverflow;

      File->read(&size, sizeof(size));
      if (size < std::ssize(buffer)) {
         if (size > 0) {
            File->read(buffer, size);
            Header.assign(buffer, size);
         }
      }
      else return ERR::BufferOverflow;

      File->read(&size, sizeof(size));
      if (size < std::ssize(buffer)) {
         if (size > 0) {
            File->read(buffer, size);
            Icon.assign(buffer, size);
         }
      }
      else return ERR::BufferOverflow;

      File->read(&size, sizeof(size));
      if (size < std::ssize(buffer)) {
         if (size > 0) {
            File->read(buffer, size);
            Description.assign(buffer, size);
         }
      }
      else return ERR::BufferOverflow;

      return ERR::Okay;
   }
};

//********************************************************************************************************************

struct ModuleHeader {
   int Total;          // Total number of registered modules
};

struct ModuleItem {
   uint32_t Hash;   // Hash of the module file name
   int  Size;       // Size of the item structure, all accompanying strings and byte alignment
   // Followed by path
};

//********************************************************************************************************************
// Memory messaging structure.

struct MemoryMessageDetail {
   int8_t buffer[4];
};

struct MemoryMessage {
   #ifdef __unix__
      long MType;                   // <-- This long field is a Linux requirement
      struct MemoryMessageDetail Detail;
   #else
      int MemoryID;
   #endif
};

//********************************************************************************************************************
// Global data variables.

extern extMetaClass glMetaClass;
extern int glEUID, glEGID, glUID, glGID;
extern std::string glSystemPath;
extern std::string glModulePath;
extern std::string glRootPath;
extern std::string glDisplayDriver;
extern bool glShowIO, glShowPrivate, glEnableCrashHandler;
extern bool glJanitorActive;
extern CONTYPE glConsoleType;
extern std::array<LogCallbackSlot, LC_LIMIT> glLogCallbacks;
extern std::atomic_uint8_t glLogCallbackCount;
extern bool glLogThreads;
extern int16_t glLogLevel, glMaxDepth;
extern TSTATE glTaskState;
extern int64_t glTimeLog;
extern RootModule *glModuleList;    // Locked with glmGeneric.  Maintained as a linked-list; hashmap unsuitable.
extern OpenInfo glOpenInfo;         // Read-only.  The OpenInfo structure initially passed to OpenCore()
extern extTask *glCurrentTask;
extern "C" const ActionTable ActionTable[];
extern const Function    glFunctions[];
extern std::list<CoreTimer> glTimers;           // Locked with glmTimer
extern ankerl::unordered_dense::map<std::string, struct ModHeader *> glStaticModules;
extern ankerl::unordered_dense::map<CLASSID, extClassRecord> glClassDB; // Class DB populated either by static_modules.cpp or by pre-generated file if modular.
extern ankerl::unordered_dense::map<CLASSID, extMetaClass *> glClassMap;
extern ankerl::unordered_dense::map<uint32_t, std::string> glFields; // Reverse lookup for converting field hashes back to their respective names.
extern std::set<std::shared_ptr<std::jthread>> glAsyncThreads;
extern OBJECTLOOKUP glObjectLookup;  // Locked with glmObjectlookup
extern std::unordered_map<MEMORYID, PrivateAddress> glPrivateMemory;  // Locked with glmMemory: Using ankerl::unordered_dense for superior performance
extern std::unordered_map<RESOURCEID, ResourceRecord> glResources; // Locked with glmMemory.
extern std::unordered_map<OBJECTID, ObjectRecord> glObjects; // Locked with glmMemory.
extern std::unordered_map<OBJECTID, ObjectSignal> glWFOList;
extern std::map<std::string, ConfigKeys, CaseInsensitiveMap> glVolumes; // VolumeName = { Key, Value }
extern std::unordered_multimap<uint32_t, CLASSID> glWildClassMap; // Fast lookup for identifying classes by file extension
extern int glWildClassMapTotal;
extern std::vector<TaskRecord> glTasks;
extern const CSTRING glMessages[int(ERR::END)+1];       // Read-only table of error messages.
extern const int glTotalMessages;
extern "C" int glProcessID;   // Read only
extern HOSTHANDLE glConsoleFD;
extern FILE *glLogFile;
extern int glStdErrFlags; // Read only
extern int glValidateProcessID; // Used by core thread only.
extern size_t glPageSize;
extern std::atomic_int glMessageIDCount;
extern std::atomic_int glGlobalIDCount;
extern std::atomic_int glPrivateIDCounter;
extern int16_t glCrashStatus, glCodeIndex, glLastCodeIndex, glSystemState;
extern std::atomic_ushort glFunctionID;
extern "C" int8_t glProgramStage;
extern bool glPrivileged, glSync;
extern TIMER glCacheTimer;
extern APTR glJNIEnv;
extern class extObjectContext glTopContext; // Read-only, not a threading concern.
extern objTime *glTime;
extern objFile *glClassFile;
extern Object glDummyObject;
extern TIMER glProcessJanitor;
extern int glEventMask;
extern struct ModHeader glCoreHeader;
#ifndef KOTUKU_STATIC
extern CSTRING glClassBinPath;
#endif

extern objMetaClass *glRootModuleClass;
extern objMetaClass *glModuleClass;
extern objMetaClass *glTaskClass;
extern objMetaClass *glThreadClass;
extern objMetaClass *glTimeClass;
extern objMetaClass *glConfigClass;
extern objMetaClass *glFileClass;
extern objMetaClass *glStorageClass;
extern objMetaClass *glScriptClass;
extern objMetaClass *glArchiveClass;
extern objMetaClass *glCompressionClass;
extern objMetaClass *glCompressedStreamClass;
#ifdef __ANDROID__
extern objMetaClass *glAssetClass;
#endif
extern int8_t fs_initialised;
extern APTR glPageFault;
extern bool glScanClasses;
extern uint8_t glTimerCycle;
extern bool glDebugMemory;
extern struct CoreBase *LocalCoreBase;
extern std::atomic_int glUniqueMsgID;

//********************************************************************************************************************
// Thread specific variables - these do not require locks.

#if defined(__MINGW32__) || defined(__MINGW64__)
// MinGW TLS destructor bug workaround: use a thread-local pointer and lazy init to avoid non-trivial TLS dtors
extern thread_local kt::vector<ObjectContext> *tlContextPtr;

static inline kt::vector<ObjectContext> & tls_get_context() noexcept
{
   if (!tlContextPtr) {
      auto p = new kt::vector<ObjectContext>();
      p->reserve(16);
      p->emplace_back(ObjectContext { &glDummyObject, nullptr, AC::NIL });
      tlContextPtr = p;
   }
   return *tlContextPtr;
}

#define tlContext (tls_get_context())

#else
extern thread_local kt::vector<ObjectContext> tlContext;
#endif
extern thread_local class TaskMessage *tlCurrentMsg;
extern thread_local bool tlMainThread;
extern thread_local int16_t tlMsgRecursion;
extern thread_local int16_t tlDepth;
extern thread_local int16_t tlLogStatus;
extern thread_local int16_t tlPreventSleep;
extern thread_local int16_t tlPublicLockCount;
extern thread_local int16_t tlPrivateLockCount;
extern thread_local int glForceUID, glForceGID;
extern thread_local PERMIT glDefaultPermissions;
extern THREADID glMainThreadID;

//********************************************************************************************************************

extern ERR (*glMessageHandler)(struct Message *);
extern void (*glVideoRecovery)(void);
extern void (*glKeyboardRecovery)(void);

#ifdef _WIN32
extern "C" WINHANDLE glProcessHandle;
extern thread_local bool tlMessageBreak;
extern WINHANDLE glTaskLock;
#endif

#ifdef __unix__
extern thread_local int glSocket;
extern int glChildSignalFD[2];
extern struct FileMonitor *glFileMonitor;
#endif

//********************************************************************************************************************
// Message handler chain structure.

extern struct MsgHandler *glMsgHandlers, *glLastMsgHandler;

class TaskMessage {
   public:
   // struct Message - START
   int64_t Time;
   int     UID;
   MSGID   Type;
   int     Size;
   // struct Message - END
   private:
   char *ExtBuffer;
   std::array<char, 64> Buffer;

   // Constructors

   public:
   TaskMessage() : Time(0), UID(0), Type(MSGID::NIL), Size(0), ExtBuffer(nullptr) { }

   TaskMessage(MSGID MsgType, APTR Data = nullptr, int DataSize = 0) {
      Time = PreciseTime();
      UID  = ++glUniqueMsgID;
      Type = MsgType;
      Size = 0;
      ExtBuffer = nullptr;
      if ((Data) and (DataSize)) setBuffer(Data, DataSize);
   }

   ~TaskMessage() {
      if (ExtBuffer) { delete[] ExtBuffer; ExtBuffer = nullptr; }
   }

   // Move constructor
   TaskMessage(TaskMessage &&Other) noexcept :
      Time(Other.Time), UID(Other.UID), Type(Other.Type), Size(Other.Size), ExtBuffer(Other.ExtBuffer) {
      if ((not ExtBuffer) and (Size)) copymem(Other.Buffer.data(), Buffer.data(), Size);
      Other.Size = 0;
      Other.ExtBuffer = nullptr;
   }

   // Copy constructor
   TaskMessage(const TaskMessage &Other) {
      ExtBuffer = nullptr;
      copy_from(Other);
   }

   // Move assignment
   TaskMessage& operator=(TaskMessage &&Other) noexcept {
      if (this IS &Other) return *this;

      if (ExtBuffer) { delete[] ExtBuffer; ExtBuffer = nullptr; }

      Time = Other.Time;
      UID  = Other.UID;
      Type = Other.Type;
      Size = Other.Size;
      ExtBuffer = Other.ExtBuffer;

      if ((not ExtBuffer) and (Size)) copymem(Other.Buffer.data(), Buffer.data(), Size);

      Other.Size = 0;
      Other.ExtBuffer = nullptr;
      return *this;
   }

   // Copy assignment
   TaskMessage& operator=(const TaskMessage& Other) {
      if (this IS &Other) return *this;
      copy_from(Other);
      return *this;
   }

   // Public methods

   char * getBuffer() { return ExtBuffer ? ExtBuffer : Buffer.data(); }

   void setBuffer(APTR Data, size_t Size) {
      if (ExtBuffer) { delete[] ExtBuffer; ExtBuffer = nullptr; }

      if (Size <= Buffer.size()) copymem(Data, Buffer.data(), Size);
      else {
         ExtBuffer = new (std::nothrow) char[Size];
         if (ExtBuffer) copymem(Data, ExtBuffer, Size);
      }

      this->Size = Size;
   }

   private:
   inline void copy_from(const TaskMessage &Source) {
      Time = Source.Time;
      UID  = Source.UID;
      Type = Source.Type;
      Size = Source.Size;
      if (Source.ExtBuffer) setBuffer(Source.ExtBuffer, Size);
      else {
         if (ExtBuffer) { delete[] ExtBuffer; ExtBuffer = nullptr; }
         if (Size) copymem(Source.Buffer.data(), Buffer.data(), Size);
      }
   }
};

//********************************************************************************************************************
// ObjectContext is used to represent the object that has the current context in terms of the run-time call stack.
// It is primarily used for the resource tracking of newly allocated memory and objects, as well as for message logs
// and analysis of the call stack.

class extObjectContext : public ObjectContext {
   public:
   inline extObjectContext() noexcept { // Dummy initialisation
      obj    = &glDummyObject;
      field  = nullptr;
      action = AC::NIL;
   }

   inline extObjectContext(OBJECTPTR pObject, AC pAction) noexcept {
      tlContext.emplace_back(pObject, nullptr, pAction);

      obj    = pObject;
      field  = nullptr;
      action = pAction;
   }

   inline extObjectContext(OBJECTPTR pObject, struct Field *pField = nullptr) noexcept {
      tlContext.emplace_back(pObject, pField, AC::NIL);

      obj    = pObject;
      field  = pField;
      action = AC::NIL;
   }

   inline extObjectContext(OBJECTPTR pObject, struct Field *pField, AC pAction) noexcept {
      tlContext.emplace_back(pObject, pField, pAction);

      obj    = pObject;
      field  = pField;
      action = pAction;
   }

   inline ~extObjectContext() noexcept {
      // Pop the context frame we pushed in the constructor
      tlContext.pop_back();
   }
};

[[maybe_unused]] static inline OBJECTPTR current_resource()
{
   // Field contexts are not treated as resource nodes (i.e. we want GetField to track to the caller)
   for (auto it=tlContext.rbegin(); it != tlContext.rend(); ++it) {
      if (not it->field) return it->obj;
   }
   return &glDummyObject;
}

[[maybe_unused]] static inline OBJECTPTR current_action()
{
   for (auto it=tlContext.rbegin(); it != tlContext.rend(); ++it) {
      if (it->action != AC::NIL) return it->obj;
   }
   return &glDummyObject;
}

//********************************************************************************************************************

#ifdef __ANDROID__
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "Kotuku:Core", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Kotuku:Core", __VA_ARGS__)
#endif

//********************************************************************************************************************
// File Descriptor table for RegisterFD()

struct FDRecord {
   HOSTHANDLE FD;                         // The file descriptor that is managed by this record.
   void (*Routine)(HOSTHANDLE, APTR);     // The routine that will process read/write messages for the FD.
   APTR Data;                             // A user specific data pointer.
   RFD  Flags;                            // Set to RFD::READ, RFD::WRITE or RFD::EXCEPT.

   FDRecord(HOSTHANDLE pFD, void (*pRoutine)(HOSTHANDLE, APTR), APTR pData, RFD pFlags) :
      FD(pFD), Routine(pRoutine), Data(pData), Flags(pFlags) { }
};

extern std::list<FDRecord> glFDTable;

#ifdef __linux__
extern int glInotify;
extern std::mutex glmInotifyLookup;
extern std::unordered_map<int, OBJECTID> glInotifyLookup;
#endif

extern int8_t glFDProtected;
extern std::vector<FDRecord> glRegisterFD;

#define LRT_Exclusive 1

//********************************************************************************************************************
// The RootModule class is used to represent the first instantation of a loaded module library.  It is managed
// internally.  Clients interface with modules via the Module class.

class RootModule : public Object {
   public:
   class RootModule *Next;     // Next module in list
   class RootModule *Prev;     // Previous module in list
   struct ModHeader *Header;   // Pointer to module header - for memory resident modules only.
   struct CoreBase *CoreBase;  // Module's personal Core reference
   #ifdef __unix__
      APTR LibraryBase;        // Module code
   #else
      MODHANDLE LibraryBase;
   #endif
   std::string Name;       // Name of the module (as declared by the header)
   struct ModHeader *Table;
   int16_t    Version;
   int16_t    OpenCount;          // Amount of programs with this module open
   float      ModVersion;          // Version of this module
   MHF        Flags;
   bool       NoUnload;
   bool       DLL;                 // TRUE if the module is a Windows DLL
   ModInit    Init;
   ModClose   Close;
   ModOpen    Open;
   ModExpunge Expunge;
   ModTest    Test;
   struct ActionEntry prvActions[int(AC::END)]; // Action routines to be intercepted by the program
   std::string LibraryName; // Name of the library loaded from disk

   RootModule() = default;
};

THREADID get_thread_id(void);
void deregister_thread(void);
[[nodiscard]] std::shared_ptr<ThreadRecord> get_thread_record(void);
ERR WakeThread(int Thread, int Stop = false);

//********************************************************************************************************************

ERR fs_closedir(DirInfo *);
ERR fs_createlink(std::string_view, std::string_view);
ERR fs_delete(std::string_view, FUNCTION *);
ERR fs_getinfo(std::string_view, FileInfo &);
ERR fs_getdeviceinfo(std::string_view, objStorageDevice *);
void fs_ignore_file(class extFile *);
ERR fs_makedir(std::string_view, PERMIT);
ERR fs_opendir(DirInfo *);
ERR fs_readlink(std::string_view, STRING *);
ERR fs_rename(std::string_view, std::string_view);
ERR fs_samefile(std::string_view, std::string_view);
ERR fs_scandir(DirInfo *);
ERR fs_testpath(std::string &, RSF, LOC *);
ERR fs_watch_path(class extFile *);

virtual_drive get_fs(std::string_view Path);
void  free_storage_class(void);

ERR    convert_zip_error(struct z_stream_s *, int);
ERR    check_cache(OBJECTPTR, int64_t, int64_t);
ERR    fs_copy(std::string_view, std::string_view, FUNCTION *, bool);
ERR    fs_copydir(std::string &, std::string &, FileFeedback *, FUNCTION *, int8_t);
PERMIT get_parent_permissions(std::string_view, int *, int *);
ERR    RenameVolume(const std::string_view &, const std::string_view &);
ERR    findfile(std::string &);
PERMIT convert_fs_permissions(int);
int   convert_permissions(PERMIT);
extern "C" ERR convert_errno(int Error, ERR Default);
void free_file_cache(void);

__export void Expunge(int16_t);

extern void add_archive(class extCompression *);
extern void remove_archive(class extCompression *);

void   print_diagnosis(int);
CSTRING action_name(OBJECTPTR Object, int ActionID);
#ifndef KOTUKU_STATIC
APTR   build_jump_table(const Function *);
#endif
void   stop_async_actions(void);
ERR    copy_args(const FunctionField *, int, int8_t *, std::vector<int8_t> &);
ERR    create_archive_volume(void);
void   dispatch_queued_action(OBJECTID);
ERR    delete_tree(std::string &, FUNCTION *, FileFeedback *);
struct ClassItem * find_class(CLASSID);
ERR    find_private_object_entry(OBJECTID, int *);
ResourceRecord * find_resource(RESOURCEID);
void   free_events(void);
void   free_module_entry(RootModule *);
void   free_wakelocks(void);
void   init_metaclass(void);
ERR    init_sleep(THREADID, int, int);
Field * lookup_id(OBJECTPTR, uint32_t, OBJECTPTR *);
ERR    msg_event(APTR, int, int, APTR, int);
ERR    msg_threadcallback(APTR, int, int, APTR, int);
ERR    msg_threadaction(APTR, int, int, APTR, int);
ERR    msg_free(APTR, int, int, APTR, int);
void   optimise_write_field(Field &);
void   PrepareSleep(void);
void   process_child_signals(HOSTHANDLE, APTR);
ERR    process_janitor(OBJECTID, int, int);
void   register_sleep(int);
void   deregister_sleep(void);
void   remove_process_waitlocks(void);
ERR    TrackResource(RESOURCEID, APTR, OBJECTID, ResourceManager *, MEM, uint32_t);
void   UntrackResource(RESOURCEID);
CLASSID lookup_class_by_ext(CLASSID, std::string_view);
ERR get_file_info(const std::string_view &Path, FileInfo &Info);

#ifndef KOTUKU_STATIC
void   scan_classes(void);
#endif

ERR  writeval_default(OBJECTPTR, Field *, int, const void *, int);
ERR  check_paths(std::string_view, PERMIT);
extern "C" ERR validate_process(int);

#ifdef _WIN32
   extern "C" WINHANDLE get_threadlock(void);
   extern "C" ERR  open_public_waitlock(WINHANDLE *, CSTRING);
   extern "C" void free_threadlocks(void);
   extern "C" ERR  wake_waitlock(WINHANDLE, int);
   extern "C" ERR  alloc_public_waitlock(WINHANDLE *, const char *Name);
   extern "C" void free_public_waitlock(WINHANDLE);
   extern "C" ERR  send_thread_msg(WINHANDLE, int Type, APTR, int);
   extern "C" int sleep_waitlock(WINHANDLE, int);
#else
   struct sockaddr_un * get_socket_path(int, socklen_t *);
   ERR alloc_public_cond(CONDLOCK *, ALF);
   void  free_public_cond(CONDLOCK *);
   ERR public_cond_wait(THREADLOCK *, CONDLOCK *, int);
   ERR send_thread_msg(int, int, APTR, int);
#endif

#ifdef _WIN32
extern "C" CONTYPE activate_console(int8_t);
extern "C" void free_threadlock(void);
extern "C" int winCheckProcessExists(int);
extern "C" int winCloseHandle(WINHANDLE);
extern "C" int winCreatePipe(WINHANDLE *Read, WINHANDLE *Write);
extern "C" int winCreateSharedMemory(STRING, int, int, WINHANDLE *, APTR *);
extern "C" WINHANDLE winCreateThread(APTR Function, APTR Arg, int StackSize, int *ID);
extern "C" APTR winAllocProtectedMemory(size_t Size, int ProtectionFlags);
extern "C" int winFreeProtectedMemory(APTR Address, size_t Size);
extern "C" size_t winGetPageSize(void);
extern "C" int winProtectMemory(APTR Address, size_t Size, bool, bool, bool);
extern "C" int winGetCurrentThreadId(void);
extern "C" void winDeathBringer(int Value);
extern "C" int winDuplicateHandle(int, int, int, int *);
extern "C" void winEnterCriticalSection(APTR);
extern std::string winFormatMessage(int);
extern "C" int winFreeLibrary(WINHANDLE);
extern "C" void winFreeProcess(APTR);
extern "C" void winGetEnv(CSTRING, std::string &);
extern "C" int winGetExeDirectory(int, STRING);
extern "C" int winGetCurrentDirectory(int, STRING);
extern "C" WINHANDLE winGetCurrentProcess(void);
extern "C" int winGetCurrentProcessId(void);
extern "C" int winGetExitCodeProcess(WINHANDLE, int *Code);
extern "C" size_t winGetFileSize(STRING);
extern "C" size_t winGetProcessMemoryUsage(int ProcessID);
extern "C" APTR winGetProcAddress(WINHANDLE, std::string_view);
extern "C" WINHANDLE winGetStdInput(void);
extern "C" void winInitialise(int *, void *);
extern "C" void winInitializeCriticalSection(APTR Lock);
extern "C" int winIsDebuggerPresent(void);
extern "C" void winDeleteCriticalSection(APTR Lock);
extern "C" int winLaunchProcess(APTR, STRING, STRING, int8_t Group, int8_t Redirect, APTR *ProcessResult, int8_t, STRING, STRING, int *);
extern "C" void winLeaveCriticalSection(APTR);
extern "C" WINHANDLE winLoadLibrary(CSTRING);
extern "C" void winLowerPriority(void);
extern "C" int winGetProcessPriority(void);
extern "C" int winSetProcessPriority(int Priority);
extern "C" int64_t winGetProcessAffinityMask(void);
extern "C" int winSetProcessAffinityMask(int64_t AffinityMask);
extern "C" void winProcessMessages(void);
extern "C" int winReadStd(APTR, int, APTR Buffer, int *Size);
extern "C" int winReadPipe(WINHANDLE FD, APTR Buffer, int *Size);
extern "C" void winResetStdOut(APTR, APTR Buffer, int *Size);
extern "C" void winResetStdErr(APTR, APTR Buffer, int *Size);
extern "C" int winWritePipe(WINHANDLE FD, CPTR Buffer, int *Size);
extern "C" void winSelect(WINHANDLE FD, char *Read, char *Write);
extern "C" void winSetEnv(CSTRING, CSTRING);
extern "C" void winSetUnhandledExceptionFilter(int (*Function)(int, APTR, int, int *));
extern "C" void winShutdown(void);
extern "C" int winTerminateApp(int dwPID, int dwTimeout);
extern "C" void winTerminateThread(WINHANDLE);
extern "C" int winTryEnterCriticalSection(APTR);
extern "C" int winUnmapViewOfFile(APTR);
extern "C" int winWaitForSingleObject(WINHANDLE, int);
extern "C" int winWaitForObjects(int Total, WINHANDLE *, int Time, int8_t WinMsgs);
extern "C" int winWaitThread(WINHANDLE, int);
extern "C" int winWriteStd(APTR, CPTR Buffer, int Size);
extern "C" int winDeleteFile(char *Path);
extern "C" int winCheckDirectoryExists(CSTRING);
extern "C" ERR winCreateDir(CSTRING);
extern "C" ERR winCreateLink(CSTRING Target, CSTRING Link);
extern "C" int winCurrentDirectory(STRING, int);
extern "C" int winFileInfo(CSTRING, size_t *, struct DateTime *, int8_t *);
extern "C" void winFindClose(WINHANDLE);
extern "C" void winFindNextChangeNotification(WINHANDLE);
extern "C" void winGetAttrib(CSTRING, int *);
extern "C" int8_t winGetCommand(char *, char *, int);
extern "C" int winGetFreeDiskSpace(char, int64_t *, int64_t *);
extern "C" int winGetLogicalDrives(void);
extern "C" int winGetLogicalDriveStrings(STRING, int);
extern ERR winGetVolumeInformation(STRING Volume, std::string &Label, std::string &FileSystem, int &Type);
extern "C" int winGetFullPathName(const char *Path, int PathLength, char *Output, char **NamePart);
extern "C" int winGetUserFolder(STRING, int);
extern "C" int winGetUserName(STRING, int);
extern "C" int winGetWatchBufferSize(void);
extern "C" int winValidateHandle(WINHANDLE Handle);
extern "C" int winMoveFile(STRING, STRING);
extern "C" ERR winReadChanges(WINHANDLE, APTR, int NotifyFlags, char *, int, int *);
extern "C" int winReadKey(CSTRING, CSTRING, STRING, int);
extern "C" int winReadRootKey(CSTRING, STRING, STRING, int);
extern "C" int winReadStdInput(WINHANDLE FD, APTR Buffer, int BufferSize, int *Size);
extern "C" int winScan(APTR *, STRING, std::string &, int64_t *, struct DateTime *, struct DateTime *, int8_t *, int8_t *, int8_t *, int8_t *);
extern "C" int winSetAttrib(CSTRING, int);
extern "C" int winSetEOF(CSTRING, int64_t);
extern "C" int winTestLocation(CSTRING, int8_t);
extern "C" ERR winWatchFile(int, CSTRING, APTR, WINHANDLE *, int *);
extern "C" void winFindCloseChangeNotification(WINHANDLE);
extern "C" APTR winFindDirectory(STRING, APTR *, STRING);
extern "C" APTR winFindFile(STRING, APTR *, STRING);
extern "C" int winSetFileTime(CSTRING, bool, int16_t Year, int16_t Month, int16_t Day, int16_t Hour, int16_t Minute, int16_t Second);
extern "C" int winResetDate(STRING);
extern "C" void winSetDllDirectory(CSTRING);
extern "C" void winEnumSpecialFolders(void (*callback)(CSTRING, CSTRING, CSTRING, CSTRING, int8_t));
extern "C" int winSetSystemTime(int16_t Year, int16_t Month, int16_t Day, int16_t Hour, int16_t Minute, int16_t Second);
extern ERR winGetTimeZoneInfo(std::string_view ZoneID, int StartYear, int EndYear, struct rkTimeZoneInfo &Info);

#endif

extern ERR object_free(Object *);
extern ResourceManager glResourceObject;

//********************************************************************************************************************

inline int64_t calc_timestamp(struct DateTime *Date) {
   return(Date->Second +
          ((int64_t)Date->Minute * 60LL) +
          ((int64_t)Date->Hour * 60LL * 60LL) +
          ((int64_t)Date->Day * 60LL * 60LL * 24LL) +
          ((int64_t)Date->Month * 60LL * 60LL * 24LL * 31LL) +
          ((int64_t)Date->Year * 60LL * 60LL * 24LL * 31LL * 12LL));
}

inline uint16_t reverse_word(uint16_t Value) {
    return (((Value & 0x00FF) << 8) | ((Value & 0xFF00) >> 8));
}

inline uint32_t reverse_long(uint32_t Value) {
    return (((Value & 0x000000FF) << 24) |
            ((Value & 0x0000FF00) <<  8) |
            ((Value & 0x00FF0000) >>  8) |
            ((Value & 0xFF000000) >> 24));
}

// Align a size to the system page size

inline size_t align_page_size(size_t Size) {
   return ((Size + glPageSize - 1) / glPageSize) * glPageSize;
}

//********************************************************************************************************************
// NOTE: To be called with glmObjectLookup only.  A given name/hash can be associated with multiple objects.

inline void remove_object_hash(OBJECTPTR Object)
{
   auto name_hash = kt::strihash(Object->Name);
   auto list = glObjectLookup.find(name_hash);
   if (list IS glObjectLookup.end()) return;

   auto &objects = list->second;
   std::erase(objects, Object);
   if (objects.empty()) glObjectLookup.erase(list);
}

//********************************************************************************************************************
// Binary search helper.  Requires a sorted container.

template<typename Container, typename Key, typename Compare>
typename Container::const_iterator binary_search(const Container& container, const Key& key, Compare comp) {
    unsigned floor = 0;
    unsigned ceiling = (unsigned)container.size();
    while (floor < ceiling) {
        unsigned i = (floor + ceiling) >> 1;
        if (comp(container[i], key) < 0) floor = i + 1;
        else if (comp(container[i], key) > 0) ceiling = i;
        else return container.begin() + i;
    }
    return container.end();
}
