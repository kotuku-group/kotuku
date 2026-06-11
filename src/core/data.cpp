
#define PRV_CORE_DATA TRUE

#include "defs.h"
#include <kotuku/main.h>
#include <kotuku/modules/core.h>

#ifdef __unix__
// In Unix/Linux builds it is assumed that the install location is static.  Dynamic loading is enabled
// during the build by setting the ROOT_PATH definition to a blank string.
   #ifndef _ROOT_PATH
      #define _ROOT_PATH /usr/local/
   #endif
   #ifndef _SYSTEM_PATH
      #define _SYSTEM_PATH /usr/local/share/kotuku/
   #endif
   #ifndef _MODULE_PATH
      #define _MODULE_PATH /usr/local/lib/kotuku/
   #endif
#else
// In Windows, path information is read from the registry.  If there are no registry entries, the system
// path is the program's working folder.
   #ifndef _ROOT_PATH
      #define _ROOT_PATH ""
   #endif
   #ifndef _SYSTEM_PATH
      #define _SYSTEM_PATH ""
   #endif
   #ifndef _MODULE_PATH
      #define _MODULE_PATH ""
   #endif
#endif

std::string glRootPath   = "" _ROOT_PATH "";
std::string glSystemPath = "" _SYSTEM_PATH "";
std::string glModulePath = "" _MODULE_PATH ""; // NB: This path will be updated to its resolved-form during Core initialisation.

std::string glDisplayDriver;

#ifndef KOTUKU_STATIC
CSTRING glClassBinPath = "system:config/classes.bin";
#endif
objMetaClass *glRootModuleClass  = 0;
objMetaClass *glModuleClass      = 0;
objMetaClass *glTaskClass        = 0;
objMetaClass *glThreadClass      = 0;
objMetaClass *glTimeClass        = 0;
objMetaClass *glConfigClass      = 0;
objMetaClass *glFileClass        = 0;
objMetaClass *glScriptClass      = 0;
objMetaClass *glArchiveClass     = 0;
objMetaClass *glStorageClass     = 0;
objMetaClass *glCompressionClass = 0;
objMetaClass *glCompressedStreamClass = 0;
#ifdef __ANDROID__
objMetaClass *glAssetClass = 0;
#endif
int8_t fs_initialised  = FALSE;
APTR glPageFault     = nullptr;
bool glScanClasses   = false;
bool glJanitorActive = false;
CONTYPE glConsoleType  = CONTYPE::NONE;
bool glDebugMemory   = false;
bool glEnableCrashHandler = true;
struct CoreBase *LocalCoreBase = nullptr;

// NB: During shutdown, elements in glMemory are not erased but will have their fields cleared.
// Can't use ankerl here because it's unsuitable for high-churn collections.

#ifdef RESOURCE_POOL
// Node pools must be constructed before the maps that reference them (guaranteed here by declaration order within
// this translation unit) and destroyed after them (reverse declaration order).
NodePool glMemoryNodePool, glResourcesNodePool, glObjectsNodePool;
PooledMap<MEMORYID, PrivateAddress> glMemory{0, PoolAllocator<std::pair<const MEMORYID, PrivateAddress>>(glMemoryNodePool)}; // Pointer stable collection
PooledMap<RESOURCEID, ResourceRecord> glResources{0, PoolAllocator<std::pair<const RESOURCEID, ResourceRecord>>(glResourcesNodePool)}; // Pointer stable collection
PooledMap<OBJECTID, ObjectRecord> glObjects{0, PoolAllocator<std::pair<const OBJECTID, ObjectRecord>>(glObjectsNodePool)}; // Pointer stable collection
#else
PooledMap<MEMORYID, PrivateAddress> glMemory; // Pointer stable collection
PooledMap<RESOURCEID, ResourceRecord> glResources; // Pointer stable collection
PooledMap<OBJECTID, ObjectRecord> glObjects; // Pointer stable collection
#endif


std::set<std::shared_ptr<std::jthread>> glAsyncThreads;

std::mutex glmActionQueue;
std::unordered_map<OBJECTID, std::deque<QueuedAction>> glActionQueues;
std::unordered_set<OBJECTID> glActiveAsyncObjects;
std::unordered_set<OBJECTID> glCancelledAsyncObjects;
std::unordered_map<OBJECTID, int> glAsyncObjectThreads;

std::condition_variable_any cvObjects;

std::mutex glmThreadRegistry;
std::unordered_map<int, std::shared_ptr<ThreadRecord>> glThreadRegistry;

std::list<CoreTimer> glTimers; // Locked with glmTimer.  std::list maintains stable pointers to elements.
std::list<FDRecord> glFDTable;
#ifdef __linux__
std::mutex glmInotifyLookup;
std::unordered_map<int, OBJECTID> glInotifyLookup;
#endif
#ifdef __unix__
int glChildSignalFD[2] = { -1, -1 };
#endif

std::map<std::string, ConfigKeys, CaseInsensitiveMap> glVolumes;
OBJECTLOOKUP glObjectLookup; // Name lookups

std::mutex glmPrint;
std::recursive_mutex glmMemory;
std::recursive_mutex glmResources; // For glResources
std::recursive_mutex glmObjects; // For glObjects
std::recursive_mutex glmMsgHandler;
std::recursive_mutex glmAsyncActions;
std::shared_timed_mutex glmObjectLookup; // For glObjectLookup
std::recursive_timed_mutex glmTimer;
std::timed_mutex glmClassDB;
std::shared_timed_mutex glmFieldKeys;
std::timed_mutex glmGeneric;
std::timed_mutex glmObjectLocking;
std::shared_timed_mutex glmVolumes;

ankerl::unordered_dense::map<std::string, struct ModHeader *> glStaticModules;
ankerl::unordered_dense::map<CLASSID, extClassRecord> glClassDB;
ankerl::unordered_dense::map<CLASSID, extMetaClass *> glClassMap;
std::unordered_map<OBJECTID, ObjectSignal> glWFOList;
ankerl::unordered_dense::map<uint32_t, std::string> glFields;

std::unordered_multimap<uint32_t, CLASSID> glWildClassMap;

std::vector<FDRecord> glRegisterFD;
std::vector<TaskRecord> glTasks;

class objRootModule  *glModuleList  = nullptr;
struct OpenInfo   glOpenInfo;
struct MsgHandler *glMsgHandlers = nullptr, *glLastMsgHandler = 0;

objFile *glClassFile   = nullptr;
extTask *glCurrentTask = nullptr;

APTR glJNIEnv = 0;
std::atomic_ushort glFunctionID = 3333; // IDTYPE_FUNCTION
int glStdErrFlags = 0;
TIMER glCacheTimer = 0;
int glValidateProcessID = 0;
int glProcessID = 0;
int glEUID = -1, glEGID = -1, glGID = -1, glUID = -1;
int glWildClassMapTotal = 0;
std::atomic_int glResourceID = 500;
std::atomic_int glMessageIDCount = 10000;
std::atomic_int glGlobalIDCount = 1;
int glEventMask = 0;
TIMER glProcessJanitor = 0;
uint8_t glTimerCycle = 1;
int8_t glFDProtected = 0;
std::atomic_int glUniqueMsgID = 1;
size_t glPageSize = 4096; // Overwritten on opening the Core

#ifdef __unix__
  thread_local int glSocket = -1; // Implemented as thread-local because we don't want threads other than main to utilise the messaging system.
#elif _WIN32
  WINHANDLE glProcessHandle = 0;
  WINHANDLE glTaskLock = 0;
#endif

HOSTHANDLE glConsoleFD = (HOSTHANDLE)-1; // Managed by GetResource()
FILE *glLogFile = nullptr;

int64_t glTimeLog       = 0;
int16_t glCrashStatus   = 0;
int16_t glCodeIndex     = CP_FINISHED;
int16_t glLastCodeIndex = 0;
int16_t glSystemState   = -1; // Initialisation state is -1
std::array<LogCallbackSlot, LC_LIMIT> glLogCallbacks;
std::atomic_uint8_t glLogCallbackCount = 0;
#ifndef NDEBUG
   int16_t glLogLevel = 2; // Thread global.  Default to warning level for debug builds.
#else
   int16_t glLogLevel = 0;
#endif
int16_t glMaxDepth  = 20; // Thread global
bool glShowIO       = false;
bool glShowPrivate  = false;
bool glPrivileged   = false;
bool glSync         = false;
bool glLogThreads   = false;
int8_t glProgramStage = STAGE_STARTUP;
TSTATE glTaskState    = TSTATE::RUNNING;
#ifdef __linux__
int glInotify = -1;
#endif

const struct virtual_drive glFSDefault = {
   0, 0, ":",
#ifdef _WIN32
   false,     // Windows is not case sensitive by default
#else
   true,      // Unix file systems are usually case sensitive
#endif
   fs_scandir,
   fs_rename,
   fs_delete,
   fs_opendir,
   fs_closedir,
   nullptr,
   fs_testpath,
   fs_watch_path,
   fs_ignore_file,
   fs_getinfo,
   fs_getdeviceinfo,
   nullptr,
   fs_makedir,
   fs_samefile,
   fs_readlink,
   fs_createlink
};

std::mutex glmVirtual;
ankerl::unordered_dense::map<uint32_t, virtual_drive> glVirtual;

#ifdef __unix__
struct FileMonitor *glFileMonitor = nullptr;
#endif

thread_local char tlFieldName[10]; // $12345678\0
thread_local int glForceUID = -1, glForceGID = -1;
thread_local PERMIT glDefaultPermissions = PERMIT::NIL;
thread_local int16_t tlDepth     = 0;
thread_local int16_t tlLogStatus = 1;
thread_local bool tlMainThread = false; // Will be set to TRUE on open, any other threads will remain FALSE.
THREADID glMainThreadID = THREADID(0);

Object glDummyObject;
ActionMessage *glCurrentActionMsg = nullptr;

#if defined(__MINGW32__) || defined(__MINGW64__)
thread_local kt::vector<ObjectContext> *tlContextPtr = nullptr; // Lazy init via tls_get_context()
#else
static kt::vector<ObjectContext> make_initial_context()
{
   kt::vector<ObjectContext> v;
   v.reserve(16);
   v.emplace_back(ObjectContext { &glDummyObject, nullptr, AC::NIL });
   return v;
}

thread_local kt::vector<ObjectContext> tlContext = make_initial_context();
#endif

objTime *glTime = nullptr;

thread_local int16_t tlMsgRecursion = 0;
thread_local TaskMessage *tlCurrentMsg = nullptr;

void (*glVideoRecovery)(void) = nullptr;
void (*glKeyboardRecovery)(void) = nullptr;

#ifdef __ANDROID__
static struct AndroidBase *AndroidBase = nullptr;
#endif

ResourceManager glResourceObject = {
   "Object",
   (ERR (*)(ResourceRecord &, APTR))&object_free,
   &object_add_child,
   &object_remove_child,
   true
};

//********************************************************************************************************************

#include "data_functions.c"
#include "data_errors.cpp"
