#pragma once

// Field flags for classes.  These are intended to simplify field definitions, e.g. using FDF_BYTEARRAY combines
// FD_ARRAY with FD_BYTE.  DO NOT use these for function definitions, they are not intended to be compatible.

// Sizes/Types

#define FT_POINTER  FD_POINTER
#define FT_FLOAT    FD_FLOAT
#define FT_INT      FD_INT
#define FT_DOUBLE   FD_DOUBLE
#define FT_INT64    FD_INT64
#define FT_STRING   (FD_POINTER|FD_STRING)
#define FT_UNLISTED FD_UNLISTED
#define FT_UNIT     FD_UNIT

// Class field definitions.  See core.h for all FD definitions.

#define FDF_BYTE       FD_BYTE
#define FDF_WORD       FD_WORD     // Field is word sized (16-bit)
#define FDF_INT        FD_INT      // Field is int sized (32-bit)
#define FDF_DOUBLE     FD_DOUBLE   // Field is double floating point sized (64-bit)
#define FDF_INT64      FD_INT64    // Field is large sized (64-bit)
#define FDF_POINTER    FD_POINTER  // Field is an address pointer (typically 32-bit)
#define FDF_ARRAY      FD_ARRAY    // Field is a pointer to an array
#define FDF_CPP        FD_CPP      // Field is a C++ type variant
#define FDF_PTR        FD_POINTER
#define FDF_UNIT       FD_UNIT
#define FDF_SYNONYM    FD_SYNONYM

#define FDF_UNSIGNED    FD_UNSIGNED
#define FDF_FUNCTION    FD_FUNCTION           // sizeof(FUNCTION) - use FDF_FUNCTIONPTR for sizeof(APTR)
#define FDF_FUNCTIONPTR (FD_FUNCTION|FD_POINTER)
#define FDF_STRUCT      FD_STRUCT
#define FDF_RESOURCE    FD_RESOURCE
#define FDF_OBJECT      (FD_POINTER|FD_OBJECT)   // Field refers to another object
#define FDF_OBJECTID    (FD_INT|FD_OBJECT)      // Field refers to another object by ID
#define FDF_LOCAL       (FD_POINTER|FD_LOCAL)    // Field refers to a local object
#define FDF_STRING      (FD_POINTER|FD_STRING)   // Field points to a string.  NB: Ideally want to remove the FD_POINTER as it should be redundant
#define FDF_STR         FDF_STRING
#define FDF_SCALED      FD_SCALED
#define FDF_FLAGS       FD_FLAGS                // Field contains flags
#define FDF_ALLOC       FD_ALLOC                // Field is a dynamic allocation - either a memory block or object
#define FDF_LOOKUP      FD_LOOKUP               // Lookup names for values in this field
#define FDF_READ        FD_READ                 // Field is readable
#define FDF_WRITE       FD_WRITE                // Field is writeable
#define FDF_INIT        FD_INIT                 // Field can only be written prior to Init()
#define FDF_SYSTEM      FD_SYSTEM
#define FDF_ERROR       (FD_INT|FD_ERROR)
#define FDF_RGB         (FD_RGB|FD_BYTE|FD_ARRAY)
#define FDF_R           FD_READ
#define FDF_W           FD_WRITE
#define FDF_RW          (FD_READ|FD_WRITE)
#define FDF_RI          (FD_READ|FD_INIT)
#define FDF_I           FD_INIT
#define FDF_VIRTUAL     FD_VIRTUAL
#define FDF_INTFLAGS    (FDF_INT|FDF_FLAGS)
#define FDF_FIELDTYPES  (FD_INT|FD_DOUBLE|FD_INT64|FD_POINTER|FD_UNIT|FD_BYTE|FD_ARRAY|FD_FUNCTION)

// These constants have to match the FD* constants << 32

#define TDOUBLE   0x8000000000000000LL
#define TINT      0x4000000000000000LL
#define TUNIT     0x2000000000000000LL
#define TFLOAT    0x1000000000000000LL // NB: Floats are upscaled to doubles when passed as v-args.
#define TPTR      0x0800000000000000LL
#define TINT64    0x0400000000000000LL
#define TFUNCTION 0x0200000000000000LL
#define TSTR      0x0080000000000000LL
#define TARRAY    0x0000100000000000LL
#define TSCALE    0x0020000000000000LL
#define TAGEND    0LL
#define TAGDIVERT -1LL
#define TSTRING   TSTR

namespace pf {

// FieldValue is used to simplify the initialisation of new objects.

struct FieldValue {
   uint32_t FieldID;
   int Type;
   union {
      CSTRING String;
      APTR    Pointer;
      CPTR    CPointer;
      double  Double;
      SCALE   Percent;
      int64_t Int64;
      int     Int;
   };

   //std::string not included as not compatible with constexpr
   constexpr FieldValue(uint32_t pFID, CSTRING pValue)  : FieldID(pFID), Type(FD_STRING), String(pValue) { };
   constexpr FieldValue(uint32_t pFID, int pValue)      : FieldID(pFID), Type(FD_INT), Int(pValue) { };
   constexpr FieldValue(uint32_t pFID, int64_t pValue)  : FieldID(pFID), Type(FD_INT64), Int64(pValue) { };
   constexpr FieldValue(uint32_t pFID, size_t pValue)   : FieldID(pFID), Type(FD_INT64), Int64(pValue) { };
   constexpr FieldValue(uint32_t pFID, double pValue)   : FieldID(pFID), Type(FD_DOUBLE), Double(pValue) { };
   constexpr FieldValue(uint32_t pFID, SCALE pValue)    : FieldID(pFID), Type(FD_DOUBLE|FD_SCALED), Percent(pValue) { };
   constexpr FieldValue(uint32_t pFID, const FUNCTION &pValue) : FieldID(pFID), Type(FDF_FUNCTIONPTR), CPointer(&pValue) { };
   constexpr FieldValue(uint32_t pFID, const FUNCTION *pValue) : FieldID(pFID), Type(FDF_FUNCTIONPTR), CPointer(pValue) { };
   constexpr FieldValue(uint32_t pFID, APTR pValue)     : FieldID(pFID), Type(FD_POINTER), Pointer(pValue) { };
   constexpr FieldValue(uint32_t pFID, CPTR pValue)     : FieldID(pFID), Type(FD_POINTER), CPointer(pValue) { };
   constexpr FieldValue(uint32_t pFID, CPTR pValue, int pCustom) : FieldID(pFID), Type(pCustom), CPointer(pValue) { };
};

}

namespace dmf { // Helper functions for DMF flags
inline bool has(DMF Value, DMF Flags) { return (Value & Flags) != DMF::NIL; }

[[nodiscard]] inline bool hasX(DMF Value) { return (Value & DMF::FIXED_X) != DMF::NIL; }
[[nodiscard]] inline bool hasY(DMF Value) { return (Value & DMF::FIXED_Y) != DMF::NIL; }
[[nodiscard]] inline bool hasWidth(DMF Value) { return (Value & DMF::FIXED_WIDTH) != DMF::NIL; }
[[nodiscard]] inline bool hasHeight(DMF Value) { return (Value & DMF::FIXED_HEIGHT) != DMF::NIL; }
[[nodiscard]] inline bool hasXOffset(DMF Value) { return (Value & DMF::FIXED_X_OFFSET) != DMF::NIL; }
[[nodiscard]] inline bool hasYOffset(DMF Value) { return (Value & DMF::FIXED_Y_OFFSET) != DMF::NIL; }
[[nodiscard]] inline bool hasRadiusX(DMF Value) { return (Value & DMF::FIXED_RADIUS_X) != DMF::NIL; }
[[nodiscard]] inline bool hasRadiusY(DMF Value) { return (Value & DMF::FIXED_RADIUS_Y) != DMF::NIL; }
[[nodiscard]] inline bool hasScaledX(DMF Value) { return (Value & DMF::SCALED_X) != DMF::NIL; }
[[nodiscard]] inline bool hasScaledY(DMF Value) { return (Value & DMF::SCALED_Y) != DMF::NIL; }
[[nodiscard]] inline bool hasScaledWidth(DMF Value) { return (Value & DMF::SCALED_WIDTH) != DMF::NIL; }
[[nodiscard]] inline bool hasScaledHeight(DMF Value) { return (Value & DMF::SCALED_HEIGHT) != DMF::NIL; }
[[nodiscard]] inline bool hasScaledXOffset(DMF Value) { return (Value & DMF::SCALED_X_OFFSET) != DMF::NIL; }
[[nodiscard]] inline bool hasScaledYOffset(DMF Value) { return (Value & DMF::SCALED_Y_OFFSET) != DMF::NIL; }
[[nodiscard]] inline bool hasScaledCenterX(DMF Value) { return (Value & DMF::SCALED_CENTER_X) != DMF::NIL; }
[[nodiscard]] inline bool hasScaledCenterY(DMF Value) { return (Value & DMF::SCALED_CENTER_Y) != DMF::NIL; }
[[nodiscard]] inline bool hasScaledRadiusX(DMF Value) { return (Value & DMF::SCALED_RADIUS_X) != DMF::NIL; }
[[nodiscard]] inline bool hasScaledRadiusY(DMF Value) { return (Value & DMF::SCALED_RADIUS_Y) != DMF::NIL; }

[[nodiscard]] inline bool hasAnyHorizontalPosition(DMF Value) { return (Value & (DMF::FIXED_X|DMF::SCALED_X|DMF::FIXED_X_OFFSET|DMF::SCALED_X_OFFSET)) != DMF::NIL; }
[[nodiscard]] inline bool hasAnyVerticalPosition(DMF Value) { return (Value & (DMF::FIXED_Y|DMF::SCALED_Y|DMF::FIXED_Y_OFFSET|DMF::SCALED_Y_OFFSET)) != DMF::NIL; }
[[nodiscard]] inline bool hasAnyScaledRadius(DMF Value) { return (Value & (DMF::SCALED_RADIUS_X|DMF::SCALED_RADIUS_Y)) != DMF::NIL; }
[[nodiscard]] inline bool hasAnyX(DMF Value) { return (Value & (DMF::SCALED_X|DMF::FIXED_X)) != DMF::NIL; }
[[nodiscard]] inline bool hasAnyY(DMF Value) { return (Value & (DMF::SCALED_Y|DMF::FIXED_Y)) != DMF::NIL; }
[[nodiscard]] inline bool hasAnyWidth(DMF Value) { return (Value & (DMF::SCALED_WIDTH|DMF::FIXED_WIDTH)) != DMF::NIL; }
[[nodiscard]] inline bool hasAnyHeight(DMF Value) { return (Value & (DMF::SCALED_HEIGHT|DMF::FIXED_HEIGHT)) != DMF::NIL; }
[[nodiscard]] inline bool hasAnyXOffset(DMF Value) { return (Value & (DMF::SCALED_X_OFFSET|DMF::FIXED_X_OFFSET)) != DMF::NIL; }
[[nodiscard]] inline bool hasAnyYOffset(DMF Value) { return (Value & (DMF::SCALED_Y_OFFSET|DMF::FIXED_Y_OFFSET)) != DMF::NIL; }
}

#define END_FIELD FieldArray(nullptr, 0)
#define FDEF static const struct FunctionField

//********************************************************************************************************************
// Locking system for when you have a) the object pointer and b) high confidence that it's alive.
// Otherwise use ScopedObjectLock.
//
// NOTE: Can terminate the object on release if it is marked for termination.

class ScopedObjectAccess {
   private:
      OBJECTPTR obj;

   public:
      ERR error;

      inline ScopedObjectAccess(OBJECTPTR Object);
      inline ~ScopedObjectAccess();
      inline bool granted() { return error == ERR::Okay; }
      inline void release();
};

//********************************************************************************************************************
// Lightweight shared access for when you only need to read stable/immutable object fields and prevent
// the object from being freed.  Does not acquire an exclusive lock, so it never blocks.  Suitable
// when the caller does not modify object state (or protects mutations with a separate mutex).
//
// NOTE: Can terminate the object on release if it is marked for termination.

class SharedObjectAccess {
   private:
      OBJECTPTR obj;

   public:
      ERR error;

      inline SharedObjectAccess(OBJECTPTR Object);
      inline ~SharedObjectAccess();
      inline bool granted() { return error IS ERR::Okay; }
      inline void release();
};

//********************************************************************************************************************
// Refer to Object->get() to see what this is about...

template <class T> inline int64_t FIELD_TAG()     { return 0; }
template <> inline int64_t FIELD_TAG<double>()    { return TDOUBLE; }
template <> inline int64_t FIELD_TAG<bool>()      { return TINT; }
template <> inline int64_t FIELD_TAG<int>()       { return TINT; }
template <> inline int64_t FIELD_TAG<int64_t>()   { return TINT64; }
template <> inline int64_t FIELD_TAG<uint64_t>()  { return TINT64; }
template <> inline int64_t FIELD_TAG<float>()     { return TFLOAT; }
template <> inline int64_t FIELD_TAG<OBJECTPTR>() { return TPTR; }
template <> inline int64_t FIELD_TAG<APTR>()      { return TPTR; }
template <> inline int64_t FIELD_TAG<CSTRING>()   { return TSTRING; }
template <> inline int64_t FIELD_TAG<STRING>()    { return TSTRING; }
template <> inline int64_t FIELD_TAG<SCALE>()     { return TDOUBLE|TSCALE; }

// For testing if type T can be matched to an FD flag.

template <class T> inline int FIELD_TYPECHECK()     { return FD_PTR|FD_STRUCT|FD_STRING; }
template <> inline int FIELD_TYPECHECK<double>()    { return FD_DOUBLE; }
template <> inline int FIELD_TYPECHECK<bool>()      { return FD_INT; }
template <> inline int FIELD_TYPECHECK<int>()       { return FD_INT; }
template <> inline int FIELD_TYPECHECK<int64_t>()   { return FD_INT64; }
template <> inline int FIELD_TYPECHECK<uint64_t>()  { return FD_INT64; }
template <> inline int FIELD_TYPECHECK<float>()     { return FD_FLOAT; }
template <> inline int FIELD_TYPECHECK<OBJECTPTR>() { return FD_PTR; }
template <> inline int FIELD_TYPECHECK<APTR>()      { return FD_PTR; }
template <> inline int FIELD_TYPECHECK<CSTRING>()   { return FD_STRING; }
template <> inline int FIELD_TYPECHECK<STRING>()    { return FD_STRING; }
template <> inline int FIELD_TYPECHECK<std::string>() { return FD_STRING|FD_CPP; }

//********************************************************************************************************************

struct ObjectContext {
   OBJECTPTR obj = nullptr;       // The object that currently has the operating context.
   struct Field *field = nullptr; // Set if the context is linked to a get/set field operation.  For logging purposes only.
   AC action = AC::NIL;           // Set if the context enters an action or method routine.
};

inline void RestoreObjectContext() { SetObjectContext(nullptr, nullptr, AC::NIL); }

//********************************************************************************************************************
// Header used for all objects.
// Note on object locking: If a routine's intent is to operate on the Object structure only, without mutating the
// state of the object's superset, it is not strictly necessary to acquire a lock.  Writeable values are managed with
// atomics and the rest are considered read-only, making many operating patterns safe.

struct Object { // Must be 64-bit aligned
   union {
      objMetaClass *Class;          // [Public] Class pointer
      class extMetaClass *ExtClass; // [Private] Internal version of the class pointer
   };
   APTR     ChildPrivate;        // Address for the ChildPrivate structure, if allocated
   APTR     CreatorMeta;         // The creator of the object is permitted to store a custom data pointer here.
   struct Object *Owner;         // The owner of this object
   std::atomic_uint64_t NotifyFlags; // Action subscription flags - space for 64 actions max
   int8_t   ActionDepth;         // Incremented each time an action or method is called on the object
   std::atomic_char Queue;       // Counter of locks attained by LockObject(); decremented by ReleaseObject(); not stable by design (see lock())
   std::atomic_char SleepQueue;  // For the use of LockObject() only
   std::atomic_uint8_t RefCount; // Reference counting - object cannot be freed until this reaches 0.  NB: This is not a locking mechanism!
   OBJECTID UID;                 // Unique object identifier
   std::atomic<uint32_t> Flags;  // Object flags
   std::atomic_int ThreadID;     // Managed by locking functions.  Atomic due to volatility.
   char Name[MAX_NAME_LEN];      // The name of the object.  NOTE: This value can be adjusted to ensure that the struct is always 8-bit aligned.

   // NB: This constructor is called by NewObject(), no need to call it manually from client code.

   Object() : Class(nullptr), ChildPrivate(nullptr), CreatorMeta(nullptr), Owner(nullptr), NotifyFlags(0),
      ActionDepth(0), Queue(0), SleepQueue(0), RefCount(0), UID(0), Flags(0), ThreadID(0), Name("") { }

   [[nodiscard]] inline bool initialised() { return Flags.load() & uint32_t(NF::INITIALISED); }
   [[nodiscard]] inline bool defined(NF pFlags) { return Flags.load() & uint32_t(pFlags); }
   [[nodiscard]] inline bool isSubClass();
   [[nodiscard]] inline OBJECTID ownerID() { return Owner ? Owner->UID : 0; }
   [[nodiscard]] inline CLASSID classID();
   [[nodiscard]] inline CLASSID baseClassID();
   [[nodiscard]] inline NF flags() { return NF(Flags.load()); }

   inline void setFlag(NF pFlag) {
      Flags.fetch_or(uint32_t(pFlag), std::memory_order_relaxed);
   }

   inline void clearFlag(NF pFlag) {
      Flags.fetch_and(~uint32_t(pFlag), std::memory_order_relaxed);
   }

   // Pinning an object provides a strong hint that the object is referenced by a variable, stored in a container, or needed by a thread.
   // Pinned objects will short-circuit ReleaseObject's automatic free-on-unlock feature, making it necessary to manually call freeIfReady()
   // after calls to unpin().
   // Pinning does not guarantee anything; objects can still be immediately terminated if their parent is removed.

   inline void pin() {
      #ifndef NDEBUG
      if (RefCount.load() >= 254) {
         pf::Log("pin").warning("RefCount overflow risk for object #%d (%s), count: %d", UID, className(), RefCount.load());
         DEBUG_BREAK
      }
      #endif
      RefCount++;
   }

   inline void unpin(bool FreeIfReady = false) {
      #ifndef NDEBUG
      if (RefCount.load() IS 0) {
         pf::Log("unpin").warning("Unbalanced unpin() on object #%d (%s) - RefCount is already 0.", UID, className());
         DEBUG_BREAK
      }
      #endif
      if (RefCount > 0) RefCount--;
      if (FreeIfReady) freeIfReady();
   }

   [[nodiscard]] inline bool isPinned() { return RefCount > 0; }

   inline bool freeIfReady() {
      if ((RefCount IS 0) and (Queue IS 0) and defined(NF::FREE_ON_UNLOCK)) {
         FreeResource(this->UID);
         return true;
      }
      else return false;
   }

   [[nodiscard]] CSTRING className();

   [[nodiscard]] inline bool collecting() { // Is object being freed or marked for collection?
      return defined(NF::FREE|NF::COLLECT|NF::FREE_ON_UNLOCK);
   }

   [[nodiscard]] inline bool terminating() { // Is object currently being freed?
      return defined(NF::FREE);
   }

   // Use lock() to quickly obtain an object lock without a call to LockObject().  Can fail if the object is being collected.

   inline ERR lock(int Timeout = -1) {
      #ifndef NDEBUG
      auto prev_queue = Queue.load(std::memory_order_relaxed);
      if (prev_queue < 0) {
         pf::Log("lock").warning("Queue already negative on #%d (%s), Queue: %d, ThreadID: %d, OurThread: %d",
            UID, className(), prev_queue, ThreadID.load(), pf::_get_thread_id());
         DEBUG_BREAK
      }
      #endif
      if (++Queue IS 1) {
         ThreadID = pf::_get_thread_id();
         return ERR::Okay;
      }
      else {
         if (ThreadID IS pf::_get_thread_id()) return ERR::Okay; // If this is for the same thread then it's a nested lock, so there's no issue.
         --Queue; // Restore the lock count
         return LockObject(this, Timeout); // Can fail if object is marked for collection.
      }
   }

   // Transfer ownership of the lock to the current thread.
   inline void transferLock() {
      ThreadID = pf::_get_thread_id();
   }

   inline void unlock() {
      #ifndef NDEBUG
      if (Queue.load() <= 0) {
         pf::Log("unlock").warning("Queue underflow on #%d (%s), Queue: %d, ThreadID: %d, OurThread: %d",
            UID, className(), Queue.load(), ThreadID.load(), pf::_get_thread_id());
         DEBUG_BREAK
      }
      #endif
      // Prefer to use ReleaseObject() if there are threads that need to be woken
      if ((SleepQueue > 0) or defined(NF::FREE_ON_UNLOCK)) ReleaseObject(this);
      else --Queue;
   }

   [[nodiscard]] inline bool locked() {
      return Queue > 0;
   }

   [[nodiscard]] inline bool hasOwner(OBJECTID ID) { // Return true if ID has ownership.
      auto obj = this->Owner;
      while ((obj) and (obj->UID != ID)) obj = obj->Owner;
      return obj ? true : false;
   }

   // set() support for array fields

   template <class T> ERR set(FIELD FieldID, const T *Data, size_t Elements, int Type = FIELD_TYPECHECK<T>()) {
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if (!(field->Flags & FD_ARRAY)) return ERR::FieldTypeMismatch;
         if ((!field->writeable()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         if ((field->Flags & FD_INIT) and (target->initialised()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         return field->WriteValue(target, field, FD_ARRAY|Type, Data, Elements);
      }
      else return ERR::UnsupportedField;
   }

   template <class T, std::size_t SIZE> ERR set(FIELD FieldID, const std::array<T, SIZE> &Value, int Type = FIELD_TYPECHECK<T>()) {
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if (!(field->Flags & FD_ARRAY)) return ERR::FieldTypeMismatch;
         if ((!field->writeable()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         if ((field->Flags & FD_INIT) and (target->initialised()) and (CurrentContext() != target)) return ERR::NoFieldAccess;

         return field->WriteValue(target, field, FD_ARRAY|Type, Value.data(), SIZE);
      }
      else return ERR::UnsupportedField;
   }

   template <class T> ERR set(FIELD FieldID, const std::vector<T> &Value, int Type = FIELD_TYPECHECK<T>()) {
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if (!(field->Flags & FD_ARRAY)) return ERR::FieldTypeMismatch;
         if ((!field->writeable()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         if ((field->Flags & FD_INIT) and (target->initialised()) and (CurrentContext() != target)) return ERR::NoFieldAccess;

         return field->WriteValue(target, field, FD_ARRAY|Type, const_cast<T *>(Value.data()), std::ssize(Value));
      }
      else return ERR::UnsupportedField;
   }

   template <class T> ERR set(FIELD FieldID, const pf::vector<T> &Value, int Type = FIELD_TYPECHECK<T>()) {
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if (!(field->Flags & FD_ARRAY)) return ERR::FieldTypeMismatch;
         if ((!field->writeable()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         if ((field->Flags & FD_INIT) and (target->initialised()) and (CurrentContext() != target)) return ERR::NoFieldAccess;

         if (field->Flags & FD_CPP) return field->WriteValue(target, field, FD_ARRAY|Type, &Value, std::ssize(Value));
         else return field->WriteValue(target, field, FD_ARRAY|Type, const_cast<T *>(Value.data()), std::ssize(Value));
      }
      else return ERR::UnsupportedField;
   }

   inline ERR set(FIELD FieldID, const FRGB &Value) {
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if (!(field->Flags & FD_ARRAY)) return ERR::FieldTypeMismatch;
         if ((!field->writeable()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         if ((field->Flags & FD_INIT) and (target->initialised()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         return field->WriteValue(target, field, FD_ARRAY|FD_FLOAT, &Value, 4);
      }
      else return ERR::UnsupportedField;
   }

   // set() support for numeric types

   // Bool overload: promote to int to avoid reading 4 bytes from a 1-byte bool in setval_long
   inline ERR set(FIELD FieldID, const bool Value) {
      return set(FieldID, int(Value));
   }

   template <class T> ERR set(FIELD FieldID, const T Value) requires std::integral<T> || std::floating_point<T> {
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if ((!field->writeable()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         if ((field->Flags & FD_INIT) and (target->initialised()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         return field->WriteValue(target, field, FIELD_TYPECHECK<T>(), &Value, 1);
      }
      else return ERR::UnsupportedField;
   }

   inline ERR set(FIELD FieldID, const FUNCTION *Value) {
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if ((!field->writeable()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         if ((field->Flags & FD_INIT) and (target->initialised()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         return field->WriteValue(target, field, FD_FUNCTION, Value, 1);
      }
      else return ERR::UnsupportedField;
   }

   inline ERR set(FIELD FieldID, const char *Value) {
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if ((!field->writeable()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         if ((field->Flags & FD_INIT) and (target->initialised()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         return field->WriteValue(target, field, FD_STRING, Value, 1);
      }
      else return ERR::UnsupportedField;
   }

   inline ERR set(FIELD FieldID, const unsigned char *Value) {
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if ((!field->writeable()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         if ((field->Flags & FD_INIT) and (target->initialised()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         return field->WriteValue(target, field, FD_STRING, Value, 1);
      }
      else return ERR::UnsupportedField;
   }

   inline ERR set(FIELD FieldID, const std::string &Value) {
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if ((!field->writeable()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         if ((field->Flags & FD_INIT) and (target->initialised()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         return field->WriteValue(target, field, FD_STRING, Value.c_str(), 1);
      }
      else return ERR::UnsupportedField;
   }

   inline ERR set(FIELD FieldID, const Unit *Value) {
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if ((!field->writeable()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         if ((field->Flags & FD_INIT) and (target->initialised()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         return field->WriteValue(target, field, FD_UNIT, Value, 1);
      }
      else return ERR::UnsupportedField;
   }

   inline ERR set(FIELD FieldID, const Unit &Value) {
      return set(FieldID, &Value);
   }

   // Works both for regular data pointers and function pointers if the field is defined correctly.

   inline ERR set(FIELD FieldID, const void *Value) {
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if ((!field->writeable()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         if ((field->Flags & FD_INIT) and (target->initialised()) and (CurrentContext() != target)) return ERR::NoFieldAccess;
         return field->WriteValue(target, field, FD_POINTER, Value, 1);
      }
      else return ERR::UnsupportedField;
   }

   // There are two mechanisms for retrieving object values; the first allows the value to be retrieved with an error
   // code and the value itself; the second ignores the error code and returns a value that could potentially be invalid.

   private:
   template <class T> ERR get_unit(Object *Object, struct Field &Field, T &Value) {
      SetObjectContext(Object, &Field, AC::NIL);

      ERR error = ERR::Okay;
      if (Field.Flags & (FD_DOUBLE|FD_INT64|FD_INT)) {
          Unit var(0, FD_DOUBLE);
          error = Field.GetValue(Object, &var);
          if (error IS ERR::Okay) Value = var.Value;
      }
      else error = ERR::FieldTypeMismatch;

      RestoreObjectContext();
      return error;
   }

   inline std::pair<ERR, APTR> get_field_value(Object *Object, struct Field &Field, int8_t Buffer[8], int &ArraySize) {
      if (Field.GetValue) {
         SetObjectContext(Object, &Field, AC::NIL);
         auto get_field = (ERR (*)(APTR, APTR, int &))Field.GetValue;
         auto pair = std::make_pair(get_field(Object, Buffer, ArraySize), Buffer);
         RestoreObjectContext();
         return pair;
      }
      else return std::make_pair(ERR::Okay, ((int8_t *)Object) + Field.Offset);
   }

   public:

   template <class T> ERR get(FIELD FieldID, T &Value) requires std::integral<T> || std::floating_point<T> {
      Value = 0;
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if (!field->readable()) return ERR::NoFieldAccess;

         ScopedObjectAccess objlock(target);

         auto flags = field->Flags;

         if (flags & FD_UNIT) return get_unit<T>(target, *field, Value);

         int8_t field_value[8];
         int array_size;
         auto fv = get_field_value(target, *field, field_value, array_size);

         if (flags & FD_INT)         Value = *((int *)fv.second);
         else if (flags & FD_INT64)  Value = *((int64_t *)fv.second);
         else if (flags & FD_DOUBLE) Value = *((double *)fv.second);
         else {
            if ((fv.first IS ERR::Okay) and (flags & FD_ALLOC)) FreeResource(GetMemoryID(*((APTR *)fv.second)));
            return ERR::FieldTypeMismatch;
         }
         return fv.first;
      }
      else return ERR::UnsupportedField;
   }

   inline ERR get(FIELD FieldID, std::string &Value) { // Retrieve field as a string, supports type conversion.
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if (!field->readable()) return ERR::NoFieldAccess;

         auto flags = field->Flags;
         if (flags & FD_UNIT) {
            double num;

            if (auto error = get_unit<double>(target, *field, num); error IS ERR::Okay) {
               char buffer[64];
               snprintf(buffer, sizeof(buffer), "%f", num);
               Value.assign(buffer);
            }
            else return error;
         }

         int8_t field_value[8];
         int array_size = -1;
         auto fv = get_field_value(target, *field, field_value, array_size);
         APTR data = fv.second;

         if (fv.first != ERR::Okay) return fv.first;

         if (flags & FD_ARRAY) {
            if (flags & FD_CPP) {
               array_size = ((pf::vector<int> *)data)->size();
               data = ((pf::vector<int> *)data)->data();
            }

            std::stringstream buffer;
            if (array_size IS -1) return ERR::Failed; // Array sizing not supported by this field.

            if (flags & FD_INT) {
               auto array = (int *)data;
               for (int i=0; i < array_size; i++) buffer << *array++ << ',';
            }
            else if (flags & FD_BYTE) {
               auto array = (uint8_t *)data;
               for (int i=0; i < array_size; i++) buffer << *array++ << ',';
            }
            else if (flags & FD_DOUBLE) {
               auto array = (double *)data;
               for (int i=0; i < array_size; i++) buffer << *array++ << ',';
            }

            Value = buffer.str();
            if (!Value.empty()) Value.pop_back(); // Remove trailing comma
            return ERR::Okay;
         }

         if (flags & FD_INT) {
            if (flags & FD_LOOKUP) {
               // Reading a lookup field as a string is permissible, we just return the string registered in the lookup table
               if (auto lookup = (FieldDef *)field->Arg) {
                  int v = ((int *)data)[0];
                  while (lookup->Name) {
                     if (v IS lookup->Value) {
                        Value = lookup->Name;
                        return ERR::Okay;
                     }
                     lookup++;
                  }
               }
               Value.clear();
            }
            else if (flags & FD_FLAGS) {
               if (auto lookup = (FieldDef *)field->Arg) {
                  std::stringstream buffer;
                  int v = ((int *)data)[0];
                  while (lookup->Name) {
                     if (v & lookup->Value) buffer << lookup->Name << '|';
                     lookup++;
                  }
                  Value = buffer.str();
                  if (!Value.empty()) Value.pop_back(); // Remove trailing pipe
                  return ERR::Okay;
               }
            }
            else Value = std::to_string(*((int *)data));
         }
         else if (flags & FD_INT64) {
            Value = std::to_string(*((int64_t *)data));
         }
         else if (flags & FD_DOUBLE) {
            char buffer[64];
            auto written = snprintf(buffer, sizeof(buffer), "%f", *((double *)data));
            Value.assign(buffer, written);
         }
         else if (flags & (FD_POINTER|FD_STRING)) {
            Value.assign(*((CSTRING *)data));
            if (flags & FD_ALLOC) FreeResource(GetMemoryID(*((CSTRING *)data)));
         }
         else return ERR::UnrecognisedFieldType;

         return ERR::Okay;
      }
      else return ERR::UnsupportedField;
   }

   // Retrieve a direct pointer to a string field, no-copy operation.  Result will require deallocation by the client if the field is marked with ALLOC.

   inline ERR get(FIELD FieldID, CSTRING &Value) {
      Object *target;
      Value = nullptr;
      if (auto field = FindField(this, FieldID, &target)) {
         if (!field->readable()) return ERR::NoFieldAccess;

         int8_t field_value[8];
         int array_size;
         auto fv = get_field_value(target, *field, field_value, array_size);
         if (fv.first != ERR::Okay) return fv.first;

         if ((field->Flags & FD_INT) and (field->Flags & FD_LOOKUP)) {
            // Reading a lookup field as a string is permissible, we just return the string registered in the lookup table
            if (auto lookup = (FieldDef *)field->Arg) {
               int value = ((int *)fv.second)[0];
               while (lookup->Name) {
                  if (value IS lookup->Value) {
                     Value = lookup->Name;
                     return ERR::Okay;
                  }
                  lookup++;
               }
            }
            return ERR::Okay;
         }
         else if (field->Flags & (FD_POINTER|FD_STRING)) {
            Value = *((CSTRING *)fv.second);
            return ERR::Okay;
         }
         else return ERR::FieldTypeMismatch;
      }
      else return ERR::UnsupportedField;
   }

   template <class T> ERR get(FIELD FieldID, T &Value) requires pcPointer<T> {
      Object *target;
      Value = nullptr;
      if (auto field = FindField(this, FieldID, &target)) {
         if (!field->readable()) return ERR::NoFieldAccess;

         int8_t field_value[8];
         int array_size;
         auto fv = get_field_value(target, *field, field_value, array_size);
         if (fv.first != ERR::Okay) return fv.first;

         if (field->Flags & (FD_POINTER|FD_STRING)) {
            Value = *((T *)fv.second);
            return ERR::Okay;
         }
         return ERR::FieldTypeMismatch;
      }
      else return ERR::UnsupportedField;
   }

   inline ERR get(FIELD FieldID, Unit &Value) {
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if (!field->readable()) return ERR::NoFieldAccess;

         if (field->Flags & FD_UNIT) {
            SetObjectContext(target, field, AC::NIL);
            auto get_field = (ERR (*)(APTR, Unit &))field->GetValue;
            auto error = get_field(target, Value);
            RestoreObjectContext();
            return error;
         }
         else return ERR::FieldTypeMismatch;
      }
      else return ERR::UnsupportedField;
   }

   template <class T> T get(FIELD FieldID)
   requires pcPointer<T> || std::integral<T> || std::floating_point<T> {
      T result(0);
      get(FieldID, result);
      return result;
   };

   template <class T> T get(FIELD FieldID) requires std::is_enum_v<T> {
      std::underlying_type_t<T> result{};
      get(FieldID, result);
      return T(result);
   };

   // Fetch an array field.  Result is a direct pointer to the data, do not free it.  Elements is set to the number of elements

   template <class T> ERR get(FIELD FieldID, T * &Result, int &Elements, bool TypeCheck = true) {
      Object *target;
      Result = nullptr;
      if (auto field = FindField(this, FieldID, &target)) {
         if ((!field->readable()) or (!(field->Flags & FD_ARRAY))) return ERR::NoFieldAccess;

         if ((TypeCheck) and (!(field->Flags & FIELD_TYPECHECK<T>()))) return ERR::FieldTypeMismatch;

         ScopedObjectAccess objlock(target);

         T *data;
         Elements = -1;

         if (field->GetValue) {
            SetObjectContext(target, field, AC::NIL);
            auto get_field = (ERR (*)(APTR, T * &, int &))field->GetValue;
            auto error = get_field(target, data, Elements);
            RestoreObjectContext();
            if (error != ERR::Okay) return error;
         }
         else if (field->Arg) { // Fixed-size array (embedded)
            Elements = field->Arg;
            data = (T *)(((int8_t *)target) + field->Offset);
         }
         else data = *((T **)((int8_t *)target) + field->Offset);

         if (field->Flags & FD_CPP) {
            auto vec = (pf::vector<APTR> *)data; // Data type doesn't matter, we just need the size().
            Result = data; // Return a generic pf::vector<>, the caller must cast to the correct type.
            Elements = vec->size();
         }
         else {
            if (Elements IS -1) return ERR::Failed;
            Result = data;
         }

         return ERR::Okay;
      }
      else return ERR::UnsupportedField;
   }

   template <typename... Args> ERR setFields(Args&&... pFields) {
      pf::Log log("setFields");

      std::initializer_list<pf::FieldValue> Fields = { std::forward<Args>(pFields)... };

      auto ctx = CurrentContext();
      for (auto &f : Fields) {
         OBJECTPTR target;
         if (auto field = FindField(this, f.FieldID, &target)) {
            if ((!(field->Flags & (FD_INIT|FD_WRITE))) and (ctx != target)) {
               log.warning("%s.%s is immutable.", className(), field->Name);
            }
            else if ((field->Flags & FD_INIT) and (target->initialised()) and (ctx != target)) {
               log.warning("%s.%s is init-only.", className(), field->Name);
            }
            else {
               if (target != this) target->lock();

               ERR error;
               if (f.Type & (FD_POINTER|FD_STRING|FD_ARRAY|FD_FUNCTION|FD_UNIT)) {
                  error = field->WriteValue(target, field, f.Type, f.Pointer, 0);
               }
               else if (f.Type & (FD_DOUBLE|FD_FLOAT)) {
                  error = field->WriteValue(target, field, f.Type, &f.Double, 1);
               }
               else if (f.Type & FD_INT64) {
                  error = field->WriteValue(target, field, f.Type, &f.Int64, 1);
               }
               else error = field->WriteValue(target, field, f.Type, &f.Int, 1);

               if (target != this) target->unlock();

               // NB: NoSupport is considered a 'soft' error that does not warrant failure.

               if ((error != ERR::Okay) and (error != ERR::NoSupport)) {
                  log.warning("%s.%s: %s", target->className(), field->Name, GetErrorMsg(error));
                  return error;
               }
            }
         }
         else return log.warning(ERR::UnsupportedField);
      }

      return ERR::Okay;
   }

} __attribute__ ((aligned (8)));

namespace pf {

// Object creation helper class.  Usage examples:
//
//   objFile::create file { fl::Path(URI), fl::Flags(FL::READ) };
//   if (file.ok()) { ... }

template<class T = Object>
class Create {
   private:
      T *obj;

   public:
      ERR error;

      // Return an unscoped direct object pointer.  NB: Globals are still tracked to their owner; use untracked() if
      // you don't want this.

      template <typename... Args> static T * global(Args&&... Fields) {
         pf::Create<T> object = { std::forward<Args>(Fields)... };
         if (object.ok()) {
            auto result = *object;
            object.obj = nullptr;
            return result;
         }
         else return nullptr;
      }

      inline static T * global(const std::initializer_list<FieldValue> Fields) {
         pf::Create<T> object(Fields);
         if (object.ok()) {
            auto result = *object;
            object.obj = nullptr;
            return result;
         }
         else return nullptr;
      }

      // Return an unscoped local object (suitable for class allocations only).

      template <typename... Args> static T * local(Args&&... Fields) {
         pf::Create<T> object({ std::forward<Args>(Fields)... }, NF::LOCAL);
         if (object.ok()) return *object;
         else return nullptr;
      }

      inline static T * local(const std::initializer_list<FieldValue> Fields) {
         pf::Create<T> object(Fields, NF::LOCAL);
         if (object.ok()) return *object;
         else return nullptr;
      }

      // Return an unscoped and untracked object pointer.

      template <typename... Args> static T * untracked(Args&&... Fields) {
         pf::Create<T> object({ std::forward<Args>(Fields)... }, NF::UNTRACKED);
         if (object.ok()) return *object;
         else return nullptr;
      }

      inline static T * untracked(const std::initializer_list<FieldValue> Fields) {
         pf::Create<T> object(Fields, NF::UNTRACKED);
         if (object.ok()) return *object;
         else return nullptr;
      }

      // Create a scoped object that is not initialised.

      Create(NF Flags = NF::NIL) : obj(nullptr), error(ERR::NewObject) {
         if (NewObject(T::CLASS_ID, Flags, (Object **)&obj) IS ERR::Okay) {
            error = ERR::Okay;
         }
      }

      // Create a scoped object that is fully initialised.

      Create(const std::initializer_list<FieldValue> Fields, NF Flags = NF::NIL) : obj(nullptr), error(ERR::Failed) {
         pf::Log log("CreateObject");
         log.branch(T::CLASS_NAME);

         if (NewObject(T::CLASS_ID, NF::SUPPRESS_LOG|Flags, (Object **)&obj) IS ERR::Okay) {
            for (auto &f : Fields) {
               OBJECTPTR target;
               if (auto field = FindField(obj, f.FieldID, &target)) {
                  if (!(field->Flags & (FD_WRITE|FD_INIT))) {
                     error = log.warning(ERR::NoFieldAccess);
                     return;
                  }
                  else {
                     target->lock();

                     if (f.Type & (FD_POINTER|FD_STRING|FD_ARRAY|FD_FUNCTION|FD_UNIT)) {
                        error = field->WriteValue(target, field, f.Type, f.Pointer, 0);
                     }
                     else if (f.Type & (FD_DOUBLE|FD_FLOAT)) {
                        error = field->WriteValue(target, field, f.Type, &f.Double, 1);
                     }
                     else if (f.Type & FD_INT64) {
                        error = field->WriteValue(target, field, f.Type, &f.Int64, 1);
                     }
                     else error = field->WriteValue(target, field, f.Type, &f.Int, 1);

                     target->unlock();

                     // NB: NoSupport is considered a 'soft' error that does not warrant failure.

                     if ((error != ERR::Okay) and (error != ERR::NoSupport)) return;
                  }
               }
               else {
                  log.warning("%s.%s field not defined.", T::CLASS_NAME, FieldName(f.FieldID));
                  error = log.warning(ERR::UndefinedField);
                  return;
               }
            }

            if ((error = InitObject(obj)) != ERR::Okay) {
               FreeResource(obj->UID);
               obj = nullptr;
            }
         }
         else error = ERR::NewObject;
      }

      ~Create() {
         if (obj) {
            if (obj->initialised()) {
               if (obj->defined(NF::UNTRACKED|NF::LOCAL))  {
                  return; // Detected a successfully created unscoped object
               }
            }
            FreeResource(obj->UID);
            obj = nullptr;
         }
      }

      T * operator->() { return obj; }; // Promotes underlying methods and fields
      T * & operator*() { return obj; }; // To allow object pointer referencing when calling functions

      inline bool ok() { return error == ERR::Okay; }

      inline T * detach() { // Return a direct pointer to the object and prevent automated destruction
         T *result = obj;
         obj = nullptr;
         return result;
      }
};

} // namespace

//********************************************************************************************************************

inline ScopedObjectAccess::ScopedObjectAccess(OBJECTPTR Object) {
   error = Object->lock();
   obj = Object;
}

inline ScopedObjectAccess::~ScopedObjectAccess() {
   if (error IS ERR::Okay) obj->unlock();
}

inline void ScopedObjectAccess::release() {
   if (error IS ERR::Okay) {
      obj->unlock();
      error = ERR::ResourceNotLocked;
   }
}

//********************************************************************************************************************

inline SharedObjectAccess::SharedObjectAccess(OBJECTPTR Object) {
   obj = Object;
   obj->pin();
   if (obj->collecting()) {
      obj->unpin();
      error = ERR::MarkedForDeletion;
   }
   else error = ERR::Okay;
}

inline SharedObjectAccess::~SharedObjectAccess() {
   if (error IS ERR::Okay) obj->unpin(true);
}

inline void SharedObjectAccess::release() {
   if (error IS ERR::Okay) {
      obj->unpin(true);
      error = ERR::ResourceNotLocked;
   }
}

//********************************************************************************************************************
// Action and Notification Structures

struct acClipboard     { static const AC id = AC::Clipboard; CLIPMODE Mode; };
struct acCopyData      { static const AC id = AC::CopyData; OBJECTPTR Dest; };
struct acDataFeed      { static const AC id = AC::DataFeed; OBJECTPTR Object; DATA Datatype; const void *Buffer; int Size; };
struct acDragDrop      { static const AC id = AC::DragDrop; OBJECTPTR Source; int Item; CSTRING Datatype; };
struct acDraw          { static const AC id = AC::Draw; int X; int Y; int Width; int Height; };
struct acGetKey        { static const AC id = AC::GetKey; CSTRING Key; STRING Value; int Size; };
struct acMove          { static const AC id = AC::Move; double DeltaX; double DeltaY; double DeltaZ; };
struct acMoveToPoint   { static const AC id = AC::MoveToPoint; double X; double Y; double Z; MTF Flags; };
struct acNewChild      { static const AC id = AC::NewChild; OBJECTPTR Object; };
struct acNewOwner      { static const AC id = AC::NewOwner; OBJECTPTR NewOwner; };
struct acRead          { static const AC id = AC::Read; APTR Buffer; int Length; int Result; };
struct acRedimension   { static const AC id = AC::Redimension; double X; double Y; double Z; double Width; double Height; double Depth; };
struct acRedo          { static const AC id = AC::Redo; int Steps; };
struct acRename        { static const AC id = AC::Rename; CSTRING Name; };
struct acResize        { static const AC id = AC::Resize; double Width; double Height; double Depth; };
struct acSaveImage     { static const AC id = AC::SaveImage; OBJECTPTR Dest; union { CLASSID ClassID; CLASSID Class; }; };
struct acSaveToObject  { static const AC id = AC::SaveToObject; OBJECTPTR Dest; union { CLASSID ClassID; CLASSID Class; }; };
struct acSeek          { static const AC id = AC::Seek; double Offset; SEEK Position; };
struct acSetKey        { static const AC id = AC::SetKey; CSTRING Key; CSTRING Value; };
struct acUndo          { static const AC id = AC::Undo; int Steps; };
struct acWrite         { static const AC id = AC::Write; CPTR Buffer; int Length; int Result; };

// Action Macros

inline ERR acActivate(OBJECTPTR Object) { return Action(AC::Activate,Object,nullptr); }
inline ERR acClear(OBJECTPTR Object) { return Action(AC::Clear,Object,nullptr); }
inline ERR acDeactivate(OBJECTPTR Object) { return Action(AC::Deactivate,Object,nullptr); }
inline ERR acDisable(OBJECTPTR Object) { return Action(AC::Disable,Object,nullptr); }
inline ERR acDraw(OBJECTPTR Object) { return Action(AC::Draw,Object,nullptr); }
inline ERR acEnable(OBJECTPTR Object) { return Action(AC::Enable,Object,nullptr); }
inline ERR acFlush(OBJECTPTR Object) { return Action(AC::Flush,Object,nullptr); }
inline ERR acFocus(OBJECTPTR Object) { return Action(AC::Focus,Object,nullptr); }
inline ERR acHide(OBJECTPTR Object) { return Action(AC::Hide,Object,nullptr); }
inline ERR acLock(OBJECTPTR Object) { return Action(AC::Lock,Object,nullptr); }
inline ERR acLostFocus(OBJECTPTR Object) { return Action(AC::LostFocus,Object,nullptr); }
inline ERR acMoveToBack(OBJECTPTR Object) { return Action(AC::MoveToBack,Object,nullptr); }
inline ERR acMoveToFront(OBJECTPTR Object) { return Action(AC::MoveToFront,Object,nullptr); }
inline ERR acNext(OBJECTPTR Object) { return Action(AC::Next,Object,nullptr); }
inline ERR acPrev(OBJECTPTR Object) { return Action(AC::Prev,Object,nullptr); }
inline ERR acQuery(OBJECTPTR Object) { return Action(AC::Query,Object,nullptr); }
inline ERR acRefresh(OBJECTPTR Object) { return Action(AC::Refresh, Object, nullptr); }
inline ERR acReset(OBJECTPTR Object) { return Action(AC::Reset,Object,nullptr); }
inline ERR acSaveSettings(OBJECTPTR Object) { return Action(AC::SaveSettings,Object,nullptr); }
inline ERR acShow(OBJECTPTR Object) { return Action(AC::Show,Object,nullptr); }
inline ERR acSignal(OBJECTPTR Object) { return Action(AC::Signal,Object,nullptr); }
inline ERR acUnlock(OBJECTPTR Object) { return Action(AC::Unlock,Object,nullptr); }

inline ERR acClipboard(OBJECTPTR Object, CLIPMODE Mode) {
   struct acClipboard args = { Mode };
   return Action(AC::Clipboard, Object, &args);
}

inline ERR acDragDrop(OBJECTPTR Object, OBJECTPTR Source, int Item, CSTRING Datatype) {
   struct acDragDrop args = { Source, Item, Datatype };
   return Action(AC::DragDrop, Object, &args);
}

inline ERR acDrawArea(OBJECTPTR Object, int X, int Y, int Width, int Height) {
   struct acDraw args = { X, Y, Width, Height };
   return Action(AC::Draw, Object, &args);
}

inline ERR acDataFeed(OBJECTPTR Object, OBJECTPTR Sender, DATA Datatype, const void *Buffer, int Size) {
   struct acDataFeed args = { Sender, Datatype, Buffer, Size };
   return Action(AC::DataFeed, Object, &args);
}

inline ERR acGetKey(OBJECTPTR Object, CSTRING Key, STRING Value, int Size) {
   struct acGetKey args = { Key, Value, Size };
   ERR error = Action(AC::GetKey, Object, &args);
   if ((error != ERR::Okay) and (Value)) Value[0] = 0;
   return error;
}

inline ERR acMove(OBJECTPTR Object, double X, double Y, double Z) {
   struct acMove args = { X, Y, Z };
   return Action(AC::Move, Object, &args);
}

inline ERR acRead(OBJECTPTR Object, APTR Buffer, int Bytes, int *Read) {
   struct acRead read = { (int8_t *)Buffer, Bytes };
   if (auto error = Action(AC::Read, Object, &read); error IS ERR::Okay) {
      if (Read) *Read = read.Result;
      return ERR::Okay;
   }
   else {
      if (Read) *Read = 0;
      return error;
   }
}

inline ERR acRedo(OBJECTPTR Object, int Steps = 1) {
   struct acRedo args = { Steps };
   return Action(AC::Redo, Object, &args);
}

inline ERR acRedimension(OBJECTPTR Object, double X, double Y, double Z, double Width, double Height, double Depth) {
   struct acRedimension args = { X, Y, Z, Width, Height, Depth };
   return Action(AC::Redimension, Object, &args);
}

inline ERR acRename(OBJECTPTR Object, CSTRING Name) {
   struct acRename args = { Name };
   return Action(AC::Rename, Object, &args);
}

inline ERR acResize(OBJECTPTR Object, double Width, double Height, double Depth) {
   struct acResize args = { Width, Height, Depth };
   return Action(AC::Resize, Object, &args);
}

inline ERR acMoveToPoint(OBJECTPTR Object, double X, double Y, double Z, MTF Flags) {
   struct acMoveToPoint moveto = { X, Y, Z, Flags };
   return Action(AC::MoveToPoint, Object, &moveto);
}

inline ERR acSaveImage(OBJECTPTR Object, OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) {
   struct acSaveImage args = { Dest, { ClassID } };
   return Action(AC::SaveImage, Object, &args);
}

inline ERR acSaveToObject(OBJECTPTR Object, OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) {
   struct acSaveToObject args = { Dest, { ClassID } };
   return Action(AC::SaveToObject, Object, &args);
}

inline ERR acSeek(OBJECTPTR Object, double Offset, SEEK Position) {
   struct acSeek args = { Offset, Position };
   return Action(AC::Seek, Object, &args);
}

inline ERR acSetKeys(OBJECTPTR Object, CSTRING tags, ...) {
   struct acSetKey args;
   va_list list;

   va_start(list, tags);
   while ((args.Key = va_arg(list, STRING)) != TAGEND) {
      args.Value = va_arg(list, STRING);
      if (auto error = Action(AC::SetKey, Object, &args); error != ERR::Okay) {
         va_end(list);
         return error;
      }
   }
   va_end(list);
   return ERR::Okay;
}

inline ERR acUndo(OBJECTPTR Object, int Steps) {
   struct acUndo args = { Steps };
   return Action(AC::Undo, Object, &args);
}

inline ERR acWrite(OBJECTPTR Object, CPTR Buffer, int Bytes, int *Result = nullptr) {
   struct acWrite write = { (int8_t *)Buffer, Bytes };
   if (auto error = Action(AC::Write, Object, &write); error IS ERR::Okay) {
      if (Result) *Result = write.Result;
      return error;
   }
   else {
      if (Result) *Result = 0;
      return error;
   }
}

inline int acWriteResult(OBJECTPTR Object, CPTR Buffer, int Bytes) {
   struct acWrite write = { (int8_t *)Buffer, Bytes };
   if (Action(AC::Write, Object, &write) IS ERR::Okay) return write.Result;
   else return 0;
}

#define acSeekStart(a,b)    acSeek((a),(b),SEEK::START)
#define acSeekEnd(a,b)      acSeek((a),(b),SEEK::END)
#define acSeekCurrent(a,b)  acSeek((a),(b),SEEK::CURRENT)

inline ERR acSetKey(OBJECTPTR Object, CSTRING Key, CSTRING Value) {
   struct acSetKey args = { Key, Value };
   return Action(AC::SetKey, Object, &args);
}
