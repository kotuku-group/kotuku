#pragma once

#define LUA_COMPILED "-- $TIRI:compiled"
constexpr int SIZE_READ = 1024;

#include <list>
#include <unordered_set>
#include <set>
#include <array>
#include <shared_mutex>
#include <kotuku/strings.hpp>
#include <kotuku/modules/regex.h>
#include <kotuku/modules/tiri.h>
#include <thread>
#include <string_view>
#include <span>
#include <concepts>

#include "lj_obj.h"
#include "lj_frame.h"
#include "lj_state.h"
#include "lauxlib.h"

using namespace pf;

template <class T> T ALIGN64(T a) { return (((a) + 7) & (~7)); }
template <class T> T ALIGN32(T a) { return (((a) + 3) & (~3)); }

extern CSTRING const glBytecodeNames[];
extern bool glPrintMsg;

//********************************************************************************************************************

[[maybe_unused]] static AET ff_to_aet(int Type)
{
   if (Type & FD_POINTER)     return AET::PTR;
   else if (Type & FD_OBJECT) return AET::STRUCT;
   else if (Type & FD_STRING) {
      if (Type & FD_CPP) return AET::STR_CPP;
      else return AET::CSTR;
   }
   else if (Type & FD_FLOAT)   return AET::FLOAT;
   else if (Type & FD_DOUBLE)  return AET::DOUBLE;
   else if (Type & FD_INT64)   return AET::INT64;
   else if (Type & FD_INT)     return AET::INT32;
   else if (Type & FD_WORD)    return AET::INT16;
   else if (Type & FD_BYTE)    return AET::BYTE;
   else return AET::MAX;
}

//********************************************************************************************************************

struct CaseInsensitiveMap {
   bool operator() (const std::string &lhs, const std::string &rhs) const {
      return ::strcasecmp(lhs.c_str(), rhs.c_str()) < 0;
   }
};

struct CaseInsensitiveHash {
   std::size_t operator()(const std::string& s) const noexcept {
      std::string lower = s;
      std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
      return std::hash<std::string>{}(lower);
   }
};

struct CaseInsensitiveEqual {
   bool operator()(const std::string& lhs, const std::string& rhs) const noexcept {
      return ::strcasecmp(lhs.c_str(), rhs.c_str()) == 0;
   }
};

struct CaseInsensitiveHashView {
   std::size_t operator()(std::string_view s) const noexcept {
      std::size_t hash = 5381;
      for (char c : s) {
         hash = ((hash << 5) + hash) + std::tolower(static_cast<unsigned char>(c));
      }
      return hash;
   }
};

struct CaseInsensitiveEqualView {
   bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
      return iequals(lhs, rhs);
   }
};

extern ankerl::unordered_dense::map<std::string_view, ACTIONID, CaseInsensitiveHashView, CaseInsensitiveEqualView> glActionLookup;
extern struct ActionTable *glActions;
extern OBJECTPTR modDisplay; // Required by tiri_input.c
extern OBJECTPTR modTiri;
extern OBJECTPTR modRegex;
extern OBJECTPTR glTiriContext;
extern OBJECTPTR clTiri;
extern JOF glJitOptions;
extern ankerl::unordered_dense::map<std::string_view, uint32_t> glStructSizes;
extern ankerl::unordered_dense::map<struct_name, struct_record, struct_hash> glStructs;

//********************************************************************************************************************
// Compile-time constant value (64-bit integer or double)

struct TiriConstant {
   enum class Type : uint8_t { Int64, Double };
   Type type;
   union {
      int64_t i64;
      double f64;
   } value;

   constexpr TiriConstant(int64_t v) : type(Type::Int64) { value.i64 = v; }
   constexpr TiriConstant(double v) : type(Type::Double) { value.f64 = v; }

   [[nodiscard]] constexpr lua_Number to_number() const {
      return (type IS Type::Int64) ? lua_Number(value.i64) : value.f64;
   }
};

// Global constant registry - case-sensitive, owns string keys
// Protected by glConstantMutex for thread-safe access
extern ankerl::unordered_dense::map<uint32_t, TiriConstant> glConstantRegistry;
extern std::shared_mutex glConstantMutex;

// Thread-safe shared pool for async.pool — see tiri_async.cpp
// Owned by the main script's prvTiri and shared with child scripts via std::shared_ptr.

enum class PoolTag : uint8_t { Number, String, Boolean, ObjectID };

struct PoolValue {
   PoolTag tag;
   union {
      double   num;
      bool     flag;
      OBJECTID oid;
   };
   std::string str; // Only valid when tag == PoolTag::String

   PoolValue() : tag(PoolTag::Number) { num = 0; }
   PoolValue(double V)       : tag(PoolTag::Number)   { num = V; }
   PoolValue(std::string V)  : tag(PoolTag::String), str(std::move(V)) { num = 0; }
   PoolValue(bool V)         : tag(PoolTag::Boolean)   { flag = V; }
   static PoolValue object(OBJECTID V) { PoolValue pv; pv.tag = PoolTag::ObjectID; pv.oid = V; return pv; }
};

struct SharedPool {
   std::shared_mutex mutex;
   ankerl::unordered_dense::map<std::string, PoolValue> data;
};

//********************************************************************************************************************
// Helper: build a std::string_view from a Lua string argument.
// Raises a Lua error if the argument at 'idx' is not a string (delegates to luaL_checklstring).

static inline std::string_view lua_checkstringview(lua_State *L, int idx)
{
   size_t len = 0;
   if (auto s = luaL_checklstring(L, idx, &len)) return std::string_view{s, len};
   else return std::string_view{};
}

// This version doesn't raise an error if the argument is not a string.

static inline std::string_view lua_tostringview(lua_State *L, int idx)
{
   size_t len = 0;
   if (auto s = lua_tolstring(L, idx, &len)) return std::string_view{s, len};
   else return std::string_view{};
}

//********************************************************************************************************************
// Standard hash computation, but stops when it encounters a character outside of A-Za-z0-9 range
// Note that struct name hashes are case sensitive.

inline uint32_t STRUCTHASH(CSTRING String)
{
   uint32_t hash = 5381;
   uint8_t c;
   while ((c = *String++)) {
      if ((c >= 'A') and (c <= 'Z'));
      else if ((c >= 'a') and (c <= 'z'));
      else if ((c >= '0') and (c <= '9'));
      else break;
      hash = ((hash<<5) + hash) + c;
   }
   return hash;
}

//********************************************************************************************************************

struct code_reader_handle {
   objFile *File;
   APTR Buffer;
};

struct actionmonitor {
   GCobject *Object;           // Native GCobject for the subscription.
   const FunctionField *Args;  // The args of the action/method are stored here so that we can build the arg value table later.
   int     Function;          // Index of function to call back.
   int     Reference;         // A custom reference to pass to the callback (optional)
   ACTIONID ActionID;          // Action being monitored.
   OBJECTID ObjectID;          // Object being monitored

   actionmonitor() {}

   ~actionmonitor() {
      if (ObjectID) {
         pf::Log log(__FUNCTION__);
         log.trace("Unsubscribe action %s from object #%d", glActions[int(ActionID)].Name, ObjectID);
         OBJECTPTR obj;
         if (AccessObject(ObjectID, 3000, &obj) IS ERR::Okay) {
            UnsubscribeAction(obj, ActionID);
            ReleaseObject(obj);
         }
      }
   }

   actionmonitor(actionmonitor &&move) noexcept :
      Object(move.Object), Args(move.Args), Function(move.Function), Reference(move.Reference), ActionID(move.ActionID), ObjectID(move.ObjectID) {
      move.ObjectID = 0;
   }

   actionmonitor& operator=(actionmonitor &&move) = default;
};

//********************************************************************************************************************

struct eventsub {
   int    Function;     // Lua function index
   EVENTID EventID;      // Event message ID
   APTR    EventHandle;

   eventsub(int pFunction, EVENTID pEventID, APTR pEventHandle) :
      Function(pFunction), EventID(pEventID), EventHandle(pEventHandle) { }

   ~eventsub() {
      if (EventHandle) UnsubscribeEvent(EventHandle);
   }

   eventsub(eventsub &&move) noexcept :
      Function(move.Function), EventID(move.EventID), EventHandle(move.EventHandle) {
      move.EventHandle = nullptr;
   }

   eventsub& operator=(eventsub &&move) = default;
};

//********************************************************************************************************************

struct datarequest {
   OBJECTID SourceID;
   int Callback;
   int64_t TimeCreated;

   datarequest(OBJECTID pSourceID, int pCallback) : SourceID(pSourceID), Callback(pCallback) {
      TimeCreated = PreciseTime();
   }
};

#include "struct_def.h"

//********************************************************************************************************************
// Variable information captured during parsing when JOF::DIAGNOSE is enabled.

struct VariableInfo {
   BCLine line;
   BCLine column;
   std::string scope;
   std::string name;
   TiriType type;
   bool is_global;
};

//********************************************************************************************************************

struct prvTiri {
   lua_State *Lua;                        // Lua instance
   std::vector<actionmonitor> ActionList; // Action subscriptions managed by subscribe()
   std::vector<eventsub> EventList;       // Event subscriptions managed by subscribeEvent()
   std::vector<datarequest> Requests;     // For drag and drop requests
   ankerl::unordered_dense::map<OBJECTID, int> StateMap;
   pf::vector<std::string> Procedures;
   std::vector<std::unique_ptr<std::jthread>> Threads; // Simple mechanism for auto-joining all the threads on object destruction
   std::shared_ptr<SharedPool> Pool;     // Thread-safe shared pool for async.pool (created on first use, shared with child scripts)
   APTR     FocusEventHandle;
   struct finput *InputList;           // Managed by the input interface
   DateTime CacheDate;
   PERMIT   CachePermissions;
   JOF      JitOptions;
   int      LoadedSize;
   int      MainChunkRef;              // Registry reference to the main chunk for post-execution analysis
   uint8_t  Recurse;
   uint8_t  SaveCompiled;
   uint16_t RequireCounter;
   std::vector<VariableInfo> CapturedVariables; // Variable declarations captured during parsing (JOF::DIAGNOSE)
};

// This structure is created & managed through the 'struct' interface

struct fstruct {
   APTR Data;          // Pointer to the structure data
   int StructSize;     // Size of the structure
   int AlignedSize;    // 64-bit alignment size of the structure.
   struct struct_record *Def; // The structure definition
   bool Deallocate;    // Deallocate the struct when Lua collects this resource.
};

struct fprocessing {
   double Timeout;
   std::list<ObjectSignal> *Signals;
};

class fregex {
public:
   Regex *regex_obj = nullptr;  // Compiled regex object
   std::string pattern;     // Original pattern string
   std::string error_msg;   // Error message if compilation failed
   REGEX flags;             // Compilation flags
   fregex(std::string_view Pattern, REGEX Flags) : pattern(Pattern), flags(Flags) { }
};

struct metafield {
   uint32_t ID;
   int GetFunction;
   int SetFunction;
};

constexpr int FIM_KEYBOARD = 1;
constexpr int FIM_DEVICE   = 2;

struct finput {
   objScript *Script;
   struct finput *Next;
   APTR   KeyEvent;
   OBJECTID SurfaceID;
   int    InputHandle;
   int    Callback;
   int    InputValue;
   JTYPE  Mask;
   int8_t Mode;
};

enum { NUM_DOUBLE=1, NUM_FLOAT, NUM_INT64, NUM_INT, NUM_INT16, NUM_BYTE };

struct fnumber { // TODO: Use std::variant
   int Type;     // Expressed as an FD_ flag.
   union {
      double  f64;
      float   f32;
      int64_t i64;
      int     i32;
      int16_t i16;
      int8_t  i8;
   };
};

struct module {
   struct Function *Functions = nullptr;
   OBJECTPTR Module = nullptr;
   ankerl::unordered_dense::map<uint32_t, int> FunctionMap; // Hash map for O(1) function lookup

   ~module() {
      if (Module) FreeResource(Module);
   }
};

constexpr uint32_t simple_hash(CSTRING String, uint32_t Hash = 5381) {
   while (auto c = *String++) Hash = ((Hash<<5) + Hash) + c;
   return Hash;
}

constexpr uint32_t char_hash(char Char, uint32_t Hash = 5381) {
   Hash = ((Hash<<5) + Hash) + Char;
   return Hash;
}

//********************************************************************************************************************

[[maybe_unused]] [[nodiscard]] constexpr CSTRING next_line(CSTRING String) noexcept
{
   if (!String) return nullptr;

   while ((*String) and (*String != '\n') and (*String != '\r')) String++;
   while (*String IS '\r') String++;
   if (*String IS '\n') String++;
   while (*String IS '\r') String++;
   if (*String) return String;
   else return nullptr;
}

//********************************************************************************************************************
// Retrieve cached read/write tables for a class (creates if not present)

READ_TABLE * get_read_table(objMetaClass *);
WRITE_TABLE * get_write_table(objMetaClass *);

//********************************************************************************************************************

struct lua_ref {
   CPTR Address;
   int Ref;
};

OBJECTPTR access_object(GCobject *);
std::vector<lua_ref> * alloc_references(void);
void load_include_for_class(lua_State *, objMetaClass *);
ERR build_args(lua_State *, const struct FunctionField *, int, int8_t *, int *);
const char * code_reader(lua_State *, void *, size_t *);
[[maybe_unused]] int code_writer_id(lua_State *, CPTR, size_t, void *);
[[maybe_unused]] int code_writer(lua_State *, CPTR, size_t, void *);
ERR create_tiri(void);
void get_line(objScript *, int, STRING, int);
APTR get_meta(lua_State *Lua, int Arg, CSTRING);
void hook_debug(lua_State *, lua_Debug *) __attribute__ ((unused));
ERR load_include(objScript *, CSTRING);
int MAKESTRUCT(lua_State *);
[[maybe_unused]] void make_any_array(lua_State *, int, std::string_view, int, CPTR);
[[maybe_unused]] void make_array(lua_State *, AET, int = 0, CPTR = nullptr, std::string_view = {});
[[maybe_unused]] ERR make_struct(objScript *, std::string_view, CSTRING);
ERR named_struct_to_table(lua_State *, std::string_view, CPTR);
void make_struct_ptr_array(lua_State *, std::string_view, int, CPTR *);
void make_struct_serial_array(lua_State *, std::string_view, int, CPTR);
void notify_action(OBJECTPTR, ACTIONID, ERR, APTR);
void process_error(objScript *, CSTRING);
ERR push_object_id(lua_State *, OBJECTID ObjectID);
extern int object_index(lua_State *);
extern int object_newindex(lua_State *);
struct fstruct * push_struct(objScript *, APTR, std::string_view, bool, bool);
struct fstruct * push_struct_def(lua_State *, APTR, struct struct_record &, bool);
extern void register_io_class(lua_State *);
extern void register_input_class(lua_State *);
extern void register_module_class(lua_State *);
extern void register_number_class(lua_State *);
extern void register_processing_class(lua_State *);
extern void register_regex_class(lua_State *);
extern void register_struct_class(lua_State *);
extern void register_async_class(lua_State *);
//static void register_widget_class(lua_State *);
void release_object(GCobject *);
void new_module(lua_State *, objModule *);
ERR struct_to_table(lua_State *, std::vector<lua_ref> &, struct struct_record &, CPTR);
ERR table_to_struct(lua_State *, std::string_view, APTR *);
ERR keyvalue_to_table(lua_State *, const KEYVALUE *);

int fcmd_arg(lua_State *);
int fcmd_msg(lua_State *);
int fcmd_print(lua_State *);
int fcmd_include(lua_State *);
int fcmd_loadfile(lua_State *);
int fcmd_exec(lua_State *);
int fcmd_subscribe_event(lua_State *);
int fcmd_unsubscribe_event(lua_State *);

#ifdef __arm__
extern void armExecFunction(APTR, APTR, int);
#else
extern void x64ExecFunction(APTR, int, int64_t *, int);
#endif

// Throws exceptions.  Used for returning objects to the user.

inline GCobject * push_object(lua_State *Lua, OBJECTPTR Object, bool Detached = true)
{
   load_include_for_class(Lua, Object->Class);
   return lua_pushobject(Lua, Object->UID, nullptr, Object->Class, Detached ? GCOBJ_DETACHED : 0);
}

//********************************************************************************************************************
// Check if we're in the immediate scope of the current try block.  This is true if the calling Lua function (one
// frame back) is the same function that contains the try block AND is at the same stack frame position.  The frame
// base check is essential for recursive functions where the same GCfunc can appear at multiple stack depths.

[[maybe_unused]] static bool in_try_immediate_scope(lua_State *L)
{
   if (L->try_stack.depth IS 0) return false;

   const TryFrame *try_frame = &L->try_stack.frames[L->try_stack.depth - 1];
   if (not try_frame->func) return false;

   TValue *current_frame = L->base - 1;
   TValue *prev_frame = frame_prev(current_frame); // Go to previous frame (the Lua caller)
   if (not prev_frame) return false;
   GCfunc *caller_func = frame_func(prev_frame); // Get the function from the previous frame
   if (not caller_func) return false;

   // Check both function identity AND frame position to handle recursive calls correctly
   if (caller_func != try_frame->func) return false;

   // The caller's base is one slot after its frame link (in LJ_FR2 mode)
   ptrdiff_t caller_base_offset = savestack(L, prev_frame + 1);
   return caller_base_offset IS try_frame->frame_base;
}

[[maybe_unused]] inline void report_action_error(lua_State *Lua, GCobject *Object, CSTRING Action, ERR Error)
{
   if ((Error >= ERR::ExceptionThreshold) and in_try_immediate_scope(Lua)) {
      luaL_error(Lua, Error, "%s.%s() failed: %s", Object->classptr->ClassName, Action, GetErrorMsg(Error));
   }
}
