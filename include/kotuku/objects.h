#pragma once

#include "kotuku/strings.hpp"

// Field flags for classes.  These are intended to simplify field definitions, e.g. using FDF_BYTEARRAY combines
// FD_ARRAY with FD_BYTE.  DO NOT use these for function definitions, they are not intended to be compatible.

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

#define FDF_CPPSTRING   (FD_CPP|FD_STRING)
#define FDF_UNSIGNED    FD_UNSIGNED
#define FDF_FUNCTION    FD_FUNCTION           // sizeof(FUNCTION) - use FDF_FUNCTIONPTR for sizeof(APTR)
#define FDF_FUNCTIONPTR (FD_FUNCTION|FD_POINTER)
#define FDF_STRUCT      FD_STRUCT
#define FDF_RESOURCE    FD_RESOURCE
#define FDF_PURE        FD_PURE                 // Getter functionality is pure - no side effects and no logging
#define FDF_OBJECT      (FD_POINTER|FD_OBJECT)  // Field refers to another object
#define FDF_OBJECTID    (FD_INT|FD_OBJECT)      // Field refers to another object by ID
#define FDF_LOCAL       (FD_POINTER|FD_LOCAL)   // Field refers to a local object
#define FDF_NORMALISED  FD_NORMALISED           // Value between 0 and 1
#define FDF_SCALED      FD_NORMALISED           // Legacy
#define FDF_FLAGS       FD_FLAGS                // Field contains flags
#define FDF_STORE       FD_STORE                // Field is a dynamic allocation - either a memory block or object
#define FDF_LOOKUP      FD_LOOKUP               // Lookup names for values in this field
#define FDF_READ        FD_READ                 // Field is readable
#define FDF_WRITE       FD_WRITE                // Field is writeable
#define FDF_INIT        FD_INIT                 // Field can only be written prior to Init()
#define FDF_SYSTEM      FD_SYSTEM
#define FDF_ERROR       (FD_INT|FD_ERROR)
#define FDF_VECTOR      (FD_CPP|FD_ARRAY)
#define FDF_SPAN        (FD_CPP|FD_BUFFER)
#define FDF_R           FD_READ
#define FDF_W           FD_WRITE
#define FDF_RW          (FD_READ|FD_WRITE)
#define FDF_RI          (FD_READ|FD_INIT)
#define FDF_I           FD_INIT
#define FDF_VIRTUAL     FD_VIRTUAL
#define FDF_INTFLAGS    (FDF_INT|FDF_FLAGS)
#define FDF_FIELDTYPES  (FD_INT|FD_DOUBLE|FD_INT64|FD_POINTER|FD_UNIT|FD_BYTE|FD_ARRAY|FD_FUNCTION)

//********************************************************************************************************************
// For testing if type T can be matched to an FD flag.  Integral types are mapped by storage width so that typed spans
// match byte, word, int and large field definitions.

template <class T> [[nodiscard]] constexpr int FIELD_TYPECHECK() {
   using field_type = std::remove_cvref_t<T>;

   if constexpr (std::is_same_v<field_type, double>) return FD_DOUBLE;
   else if constexpr (std::is_same_v<field_type, float>) return FD_FLOAT;
   else if constexpr (std::is_integral_v<field_type> and (sizeof(field_type) IS sizeof(int8_t))) return FD_BYTE;
   else if constexpr (std::is_integral_v<field_type> and (sizeof(field_type) IS sizeof(int16_t))) return FD_WORD;
   else if constexpr (std::is_integral_v<field_type> and (sizeof(field_type) IS sizeof(int64_t))) return FD_INT64;
   else if constexpr (std::is_integral_v<field_type>) return FD_INT;
   else if constexpr (std::is_same_v<field_type, std::string>) return FD_STRING|FD_CPP;
   else if constexpr (std::is_same_v<field_type, CSTRING> or std::is_same_v<field_type, STRING>) return FD_STRING;
   else if constexpr (std::is_same_v<field_type, OBJECTPTR> or std::is_same_v<field_type, APTR>) return FD_PTR;
   else return FD_PTR|FD_STRUCT|FD_STRING;
}

namespace kt {

//********************************************************************************************************************
// FieldValue is used to simplify the initialisation of new objects.

struct FieldValue {
   uint32_t FieldID; // Unique field hash
   int Type; // FD flags
   union {
      std::string_view CPPString;
      std::span<const uint8_t> Span;
      APTR    Pointer;
      CPTR    CPointer;
      double  Double;
      SCALE   Percent;
      int64_t Int64;
      int     Int;
   };

   //std::string not included as not compatible with constexpr
   constexpr FieldValue(uint32_t pFID, const std::string_view pValue) : FieldID(pFID), Type(FD_STRING|FD_CPP), CPPString(pValue) { };
   constexpr FieldValue(uint32_t pFID, int pValue)      : FieldID(pFID), Type(FD_INT), Int(pValue) { };
   constexpr FieldValue(uint32_t pFID, int64_t pValue)  : FieldID(pFID), Type(FD_INT64), Int64(pValue) { };
   constexpr FieldValue(uint32_t pFID, size_t pValue)   : FieldID(pFID), Type(FD_INT64), Int64(pValue) { };
   constexpr FieldValue(uint32_t pFID, double pValue)   : FieldID(pFID), Type(FD_DOUBLE), Double(pValue) { };
   constexpr FieldValue(uint32_t pFID, SCALE pValue)    : FieldID(pFID), Type(FD_DOUBLE|FD_SCALED), Percent(pValue) { };
   constexpr FieldValue(uint32_t pFID, const FUNCTION &pValue) : FieldID(pFID), Type(FDF_FUNCTIONPTR), CPointer(&pValue) { };
   constexpr FieldValue(uint32_t pFID, const FUNCTION *pValue) : FieldID(pFID), Type(FDF_FUNCTIONPTR), CPointer(pValue) { };
   constexpr FieldValue(uint32_t pFID, APTR pValue)     : FieldID(pFID), Type(FD_POINTER), Pointer(pValue) { };
   constexpr FieldValue(uint32_t pFID, CSTRING pValue)  : FieldID(pFID), Type(FD_STRING|FD_CPP), CPPString(pValue) { };
   constexpr FieldValue(uint32_t pFID, CPTR pValue)     : FieldID(pFID), Type(FD_POINTER), CPointer(pValue) { };
   constexpr FieldValue(uint32_t pFID, CPTR pValue, int pCustom) : FieldID(pFID), Type(pCustom), CPointer(pValue) { };

   template <class T> constexpr FieldValue(uint32_t pFID, std::span<const T> pValue)
      : FieldID(pFID), Type(FD_ARRAY | FIELD_TYPECHECK<T>()),
        Span((const uint8_t *)pValue.data(), pValue.size()) { }

   constexpr FieldValue(std::string_view pFID, const std::string_view pValue) : FieldID(kt::fieldhash(pFID)), Type(FD_STRING|FD_CPP), CPPString(pValue) { };
   constexpr FieldValue(std::string_view pFID, int pValue)      : FieldID(kt::fieldhash(pFID)), Type(FD_INT), Int(pValue) { };
   constexpr FieldValue(std::string_view pFID, int64_t pValue)  : FieldID(kt::fieldhash(pFID)), Type(FD_INT64), Int64(pValue) { };
   constexpr FieldValue(std::string_view pFID, size_t pValue)   : FieldID(kt::fieldhash(pFID)), Type(FD_INT64), Int64(pValue) { };
   constexpr FieldValue(std::string_view pFID, double pValue)   : FieldID(kt::fieldhash(pFID)), Type(FD_DOUBLE), Double(pValue) { };
   constexpr FieldValue(std::string_view pFID, SCALE pValue)    : FieldID(kt::fieldhash(pFID)), Type(FD_DOUBLE|FD_SCALED), Percent(pValue) { };
   constexpr FieldValue(std::string_view pFID, const FUNCTION &pValue) : FieldID(kt::fieldhash(pFID)), Type(FDF_FUNCTIONPTR), CPointer(&pValue) { };
   constexpr FieldValue(std::string_view pFID, const FUNCTION *pValue) : FieldID(kt::fieldhash(pFID)), Type(FDF_FUNCTIONPTR), CPointer(pValue) { };
   constexpr FieldValue(std::string_view pFID, APTR pValue)     : FieldID(kt::fieldhash(pFID)), Type(FD_POINTER), Pointer(pValue) { };
   constexpr FieldValue(std::string_view pFID, CSTRING pValue)  : FieldID(kt::fieldhash(pFID)), Type(FD_STRING|FD_CPP), CPPString(pValue) { };
   constexpr FieldValue(std::string_view pFID, CPTR pValue)     : FieldID(kt::fieldhash(pFID)), Type(FD_POINTER), CPointer(pValue) { };
   constexpr FieldValue(std::string_view pFID, CPTR pValue, int pCustom) : FieldID(kt::fieldhash(pFID)), Type(pCustom), CPointer(pValue) { };
};

inline ERR write_field_value(OBJECTPTR Target, const struct Field *FieldPtr, const FieldValue &Value);

}

#define END_FIELD FieldArray(nullptr, 0)
#define FDEF static const struct FunctionField

#if defined(KOTUKU_STATIC) or defined(PRV_CORE_MODULE)
extern "C" void ReleaseZombie(OBJECTPTR Object);
#endif

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
      ScopedObjectAccess(const ScopedObjectAccess &) = delete;
      ScopedObjectAccess & operator=(const ScopedObjectAccess &) = delete;
      inline ~ScopedObjectAccess();
      inline bool granted() const { return error IS ERR::Okay; }
      inline void release();
};

//********************************************************************************************************************
// Lightweight shared access for when you only need to read stable/immutable object fields and prevent the object
// from being freed.  Does not acquire an exclusive lock, so it never blocks.  Suitable when the caller does not
// modify object state (or protects mutations with a separate mutex).
//
// NOTE: Can terminate the object on release if it is marked for termination.

class SharedObjectAccess {
   private:
      OBJECTPTR obj;

   public:
      ERR error;

      inline SharedObjectAccess(OBJECTPTR Object);
      SharedObjectAccess(const SharedObjectAccess &) = delete;
      SharedObjectAccess & operator=(const SharedObjectAccess &) = delete;
      inline ~SharedObjectAccess();
      inline bool granted() const { return error IS ERR::Okay; }
      inline void release();
};

//********************************************************************************************************************

struct ObjectContext {
   OBJECTPTR obj = nullptr;             // The object that currently has the operating context.
   const struct Field *field = nullptr; // Set if the context is linked to a get/set field operation.  For logging purposes only.
   AC action = AC::NIL;           // Set if the context enters an action or method routine.
};

inline void RestoreObjectContext() { SetObjectContext(nullptr, nullptr, AC::NIL); }

//********************************************************************************************************************
// Header used for all objects.
// Note on object locking: If a routine's intent is to operate on the Object structure only, without mutating the
// state of the object's superset, it is not strictly necessary to acquire a lock.  Writeable values are managed with
// atomics and the rest are considered read-only, making many operating patterns safe.

struct alignas(8) Object { // Must be 64-bit aligned
   union {
      objMetaClass *Class;          // [Public] Class pointer
      class extMetaClass *ExtClass; // [Private] Internal version of the class pointer
   };
   APTR     DerivedPtr;          // [8] Private allocation area for derived classes only
   APTR     CreatorMeta;         // [16] The creator of the object is permitted to store a custom data pointer here.
   struct Object *Owner;         // [24] The owner of this object
   std::atomic_uint64_t NotifyFlags; // [32] Action subscription flags - space for 64 actions max
   std::atomic<uint32_t> RefCount; // [40] Packed pin counters: low byte strong pins, upper 24 bits weak pins.  NB: This is not a locking mechanism!
   OBJECTID UID;                 // [44] Unique object identifier
   std::atomic<uint32_t> Flags;  // [48] Object NF flags
   std::atomic_int ThreadID;     // [52] Managed by locking functions.  Atomic due to volatility.
   int8_t   ActionDepth;         // [56] Debug builds only: Incremented each time an action or method is called on the object
   std::atomic_char Queue;       // [57] Counter of locks attained by LockObject(); decremented by ReleaseObject(); not stable by design (see lock())
   std::atomic_char SleepQueue;  // [58] For the use of LockObject() only
   int8_t   _Reserved1;          // [59] Reserved
   char Name[36];                // [60] The name of the object.  NOTE: This value can be adjusted to ensure that the struct is always 8-bit aligned.
   // Refer to MAX_NAME_LEN in core.h that defines the hard limit of the name length for clients.

   Object(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept :
      Class(ClassPtr),
      DerivedPtr(nullptr),
      CreatorMeta(nullptr),
      Owner(nullptr),
      NotifyFlags(0),
      RefCount(0),
      UID(ObjectID),
      Flags(0),
      ThreadID(0),
      ActionDepth(0),
      Queue(0),
      SleepQueue(0),
      Name("") { }

   Object() = delete;

   [[nodiscard]] inline bool initialised() const { return Flags.load(std::memory_order_relaxed) & uint32_t(NF::INITIALISED); }
   [[nodiscard]] inline bool defined(NF pFlags) const { return Flags.load(std::memory_order_relaxed) & uint32_t(pFlags); }
   [[nodiscard]] inline bool isDerived();
   [[nodiscard]] inline OBJECTID ownerID() const { return Owner ? Owner->UID : 0; }
   [[nodiscard]] inline CLASSID classID();
   [[nodiscard]] inline CLASSID baseClassID();
   [[nodiscard]] inline NF flags() { return NF(Flags.load(std::memory_order_relaxed)); }
   inline void setFlag(NF pFlag) { Flags.fetch_or(uint32_t(pFlag), std::memory_order_relaxed); }
   inline void clearFlag(NF pFlag) { Flags.fetch_and(~uint32_t(pFlag), std::memory_order_relaxed); }

   // Two tiers of pinning are packed into RefCount: the low byte counts strong pins and the upper 24 bits count weak
   // pins.  Both tiers keep the object's header block allocated after termination (zombie mode) until every pin is
   // released; only the header fields Flags and RefCount remain valid at that point, and pin holders may only test
   // NF::FREE and unpin.  Placement objects cannot be pinned because the Core does not own their memory block.
   //
   // Strong pins - pin()/unpin() - additionally defer explicit termination: FreeResource() marks the object with
   // FREE_ON_UNLOCK and collection occurs via unpin(true) or freeIfReady().  Use a strong pin when the object must
   // remain operational, e.g. for the duration of an asynchronous action.
   //
   // Weak pins - pinWeak()/unpinWeak() - never impede termination and exist purely for stale-reference detection,
   // e.g. callback subscriptions.  A weak pin on an ancestor cannot deadlock its destruction.

   static constexpr uint32_t STRONG_PINS = 0x00000001; // RefCount increment per strong pin
   static constexpr uint32_t WEAK_PINS   = 0x00000100; // RefCount increment per weak pin

   inline void pin() {
      #ifndef NDEBUG
      auto ref_count = RefCount.load(std::memory_order_relaxed);
      if ((ref_count & 0xff) >= 254) {
         kt::Log("pin").warning("Strong pin overflow risk for object #%d (%s), count: %d", UID, className(), ref_count & 0xff);
         DEBUG_BREAK
      }
      #endif
      RefCount.fetch_add(STRONG_PINS, std::memory_order_relaxed);
   }

   inline void unpin(bool FreeIfReady = false) {
      // Saturate at zero; a plain load-check-decrement would allow two racing threads to underflow the counter.
      auto ref_count = RefCount.load(std::memory_order_relaxed);
      while ((ref_count & 0xff) > 0) {
         if (RefCount.compare_exchange_weak(ref_count, ref_count - STRONG_PINS, std::memory_order_acq_rel,
               std::memory_order_relaxed)) {
            if ((ref_count IS STRONG_PINS) and defined(NF::ZOMBIE)) {
               ReleaseZombie(this);
               return;
            }
            break;
         }
      }
      #ifndef NDEBUG
      if ((ref_count & 0xff) IS 0) {
         kt::Log("unpin").warning("Unbalanced unpin() on object #%d (%s) - no strong pins are held.", UID, className());
         DEBUG_BREAK
      }
      #endif
      if (FreeIfReady) freeIfReady();
   }

   inline void pinWeak() {
      #ifndef NDEBUG
      auto ref_count = RefCount.load(std::memory_order_relaxed);
      if ((ref_count >> 8) >= 0xfffffe) {
         kt::Log("pinWeak").warning("Weak pin overflow risk for object #%d (%s), count: %d", UID, className(), ref_count >> 8);
         DEBUG_BREAK
      }
      #endif
      RefCount.fetch_add(WEAK_PINS, std::memory_order_relaxed);
   }

   inline void unpinWeak() {
      // Saturate at zero on the weak byte; see unpin() for rationale.
      auto ref_count = RefCount.load(std::memory_order_relaxed);
      while ((ref_count >> 8) > 0) {
         if (RefCount.compare_exchange_weak(ref_count, ref_count - WEAK_PINS, std::memory_order_acq_rel,
               std::memory_order_relaxed)) {
            if ((ref_count IS WEAK_PINS) and defined(NF::ZOMBIE)) ReleaseZombie(this);
            return;
         }
      }
      #ifndef NDEBUG
      kt::Log("unpinWeak").warning("Unbalanced unpinWeak() on object #%d - no weak pins are held.", UID);
      DEBUG_BREAK
      #endif
   }

   // Strong pins only; weak pins never influence lock release or termination decisions.
   [[nodiscard]] inline bool isPinned() const { return (RefCount.load(std::memory_order_acquire) & 0xff) > 0; }

   [[nodiscard]] inline bool isZombie() const { return defined(NF::ZOMBIE); }

   inline bool freeIfReady() {
      auto ref_count = RefCount.load(std::memory_order_acquire) & 0xff; // Strong pins only
      auto queue = Queue.load(std::memory_order_relaxed);
      if ((ref_count IS 0) and (queue IS 0) and defined(NF::FREE_ON_UNLOCK)) {
         FreeResource(this->UID);
         return true;
      }
      else return false;
   }

   [[nodiscard]] CSTRING className();

   [[nodiscard]] inline bool collecting() const { // Is object being freed or marked for collection?
      return defined(NF::FREE|NF::FREE_ON_UNLOCK);
   }

   [[nodiscard]] inline bool terminating() const { // Is object currently being freed?
      return defined(NF::FREE);
   }

   // Use lock() to quickly obtain an object lock without a call to LockObject().  Can fail if the object is being collected.

   inline ERR lock(int Timeout = -1) {
      #ifndef NDEBUG
      auto prev_queue = Queue.load(std::memory_order_relaxed);
      if (prev_queue < 0) {
         kt::Log("lock").warning("Queue already negative on #%d (%s), Queue: %d, ThreadID: %d, OurThread: %d",
            UID, className(), prev_queue, ThreadID.load(), GetThreadID());
         DEBUG_BREAK
      }
      #endif
      if (++Queue IS 1) {
         ThreadID = GetThreadID();
         return ERR::Okay;
      }
      else {
         if (ThreadID IS GetThreadID()) return ERR::Okay; // If this is for the same thread then it's a nested lock, so there's no issue.
         --Queue; // Restore the lock count
         return LockObject(this, Timeout); // Can fail if object is marked for collection.
      }
   }

   // Transfer ownership of the lock to the current thread.
   inline void transferLock() {
      ThreadID = GetThreadID();
   }

   inline void unlock() {
      #ifndef NDEBUG
      if (Queue.load() <= 0) {
         kt::Log("unlock").warning("Queue underflow on #%d (%s), Queue: %d, ThreadID: %d, OurThread: %d",
            UID, className(), Queue.load(), ThreadID.load(), GetThreadID());
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

   [[nodiscard]] inline bool hasOwner(OBJECTID ID) const { // Return true if ID has ownership.
      auto obj = this->Owner;
      while ((obj) and (obj->UID != ID)) obj = obj->Owner;
      return obj ? true : false;
   }

   inline ERR setName(const std::string_view &Name) { return SetName(this, Name); }

   inline ERR setOwner(OBJECTID ID) {
      Object *new_owner;
      if (auto error = AccessObject(ID, 1000, &new_owner); error IS ERR::Okay) {
         error = SetOwner(this, new_owner);
         ReleaseObject(new_owner);
         return error;
      }
      else return error;
   }

   inline ERR setOwner(OBJECTPTR NewOwner) { return SetOwner(this, NewOwner); }

   public:

   // set() support for array fields

   template <class T> ERR set(const struct Field *Field, const std::span<const T> &Value, int Type = FIELD_TYPECHECK<T>()) {
      return Field->WriteValue(this, Field, FD_ARRAY|Type, &Value);
   }

   // Mutable-span callers forward to the const-span overload (the value is only read).

   template <class T> ERR set(const struct Field *Field, const std::span<T> &Value, int Type = FIELD_TYPECHECK<T>()) {
      return set(Field, std::span<const T>(Value), Type);
   }

   template <class T> ERR set(const struct Field *Field, const std::vector<T> &Value, int Type = FIELD_TYPECHECK<T>()) {
      return set(Field, std::span<const T>(Value), Type);
   }

   template <class T> ERR set(const struct Field *Field, const kt::vector<T> &Value, int Type = FIELD_TYPECHECK<T>()) {
      return set(Field, std::span<const T>(Value.data(), Value.size()), Type);
   }

   // set() support for numeric types

   // Bool overload: promote to int to avoid reading 4 bytes from a 1-byte bool in setval_long
   inline ERR set(const struct Field *Field, const bool Value) {
      return set(Field, int(Value));
   }

   template <class T> ERR set(const struct Field *Field, const T Value) requires std::integral<T> or std::floating_point<T> {
      if constexpr (std::is_integral_v<T> and (sizeof(T) < sizeof(int))) {
         // Promote small integrals to int so that WriteValue does not read 4 bytes from a 1 or 2 byte value.
         const int promoted = int(Value);
         return Field->WriteValue(this, Field, FD_INT, &promoted);
      }
      else return Field->WriteValue(this, Field, FIELD_TYPECHECK<T>(), &Value);
   }

   inline ERR set(const struct Field *Field, const FUNCTION *Value) {
      return Field->WriteValue(this, Field, FD_FUNCTION, Value);
   }

   inline ERR set(const struct Field *Field, const char *Value) {
      std::string_view sv(Value ? Value : "");
      return Field->WriteValue(this, Field, FD_STRING|FD_CPP, &sv);
   }

   inline ERR set(const struct Field *Field, std::string_view Value) {
      return Field->WriteValue(this, Field, FD_CPP|FD_STRING, &Value);
   }

   inline ERR set(const struct Field *Field, const std::string &Value) {
      auto sv = std::string_view(Value);
      return Field->WriteValue(this, Field, FD_CPP|FD_STRING, &sv);
   }

   inline ERR set(const struct Field *Field, const Unit *Value) {
      return Field->WriteValue(this, Field, FD_UNIT, Value);
   }

   inline ERR set(const struct Field *Field, const Unit &Value) {
      return set(Field, &Value);
   }

   // Works both for regular data pointers and function pointers if the field is defined correctly.

   inline ERR set(const struct Field *Field, const void *Value) {
      return Field->WriteValue(this, Field, FD_POINTER, Value);
   }

   // Internal get helpers

   private:
   template <class T> ERR get_unit(Object *Object, const struct Field &Field, T &Value) requires std::integral<T> or std::floating_point<T> {
      if (not Field.GetValue) { // No custom getter; the Unit is stored directly at the field offset
         Value = ((Unit *)(((int8_t *)Object) + Field.Offset))->Value;
         return ERR::Okay;
      }

      if (not Field.pure()) SetObjectContext(Object, &Field, AC::NIL);
      Unit var(0);
      auto error = Field.GetValue(Object, &var);
      if (error IS ERR::Okay) Value = var.Value;
      if (not Field.pure()) RestoreObjectContext();
      return error;
   }

   inline std::pair<ERR, APTR> get_field_value(Object *Object, const struct Field &Field, int8_t (&Buffer)[sizeof(std::string_view)]) {
      if (Field.GetValue) {
         SetObjectContext(Object, &Field, AC::NIL);
         auto get_field = (ERR (*)(APTR, APTR))Field.GetValue;
         auto pair = std::make_pair(get_field(Object, Buffer), Buffer);
         RestoreObjectContext();
         return pair;
      }
      else return std::make_pair(ERR::Okay, ((int8_t *)Object) + Field.Offset);
   }

   // There are two mechanisms for retrieving object values; the first allows the value to be retrieved with an error
   // code and the value itself; the second ignores the error code and returns a value that could potentially be invalid.

   public:

   template <class T> ERR get(FIELD FieldID, T &Value) requires std::integral<T> or std::floating_point<T> {
      Value = 0;
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if (not field->readable()) return ERR::NoFieldAccess;

         auto flags = field->Flags;

         if (flags & FD_UNIT) return get_unit<T>(target, *field, Value);

         int8_t field_value[sizeof(std::string_view)];
         auto fv = get_field_value(target, *field, field_value);

         if (flags & FD_INT)         Value = *((int *)fv.second);
         else if (flags & FD_INT64)  Value = *((int64_t *)fv.second);
         else if (flags & FD_DOUBLE) Value = *((double *)fv.second);
         else return ERR::FieldTypeMismatch;
         return fv.first;
      }
      else return ERR::UnsupportedField;
   }

   inline ERR get(FIELD FieldID, std::string &Value) { // Retrieve field as a string, supports type conversion.
      Object *target;
      Value.clear(); // Guarantees that no path can return a stale result in Value
      if (auto field = FindField(this, FieldID, &target)) {
         if (not field->readable()) return ERR::NoFieldAccess;

         auto flags = field->Flags;

         if (flags & FD_ARRAY) return ERR::UnsupportedField;

         if (flags & FD_UNIT) {
            double num;

            if (auto error = get_unit<double>(target, *field, num); error IS ERR::Okay) {
               char buffer[64];
               auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), num);
               if (ec IS std::errc()) Value.assign(buffer, ptr);
               return ERR::Okay;
            }
            else return error;
         }

         if ((flags & FD_STRING) and (flags & FD_STORE) and field->GetValue) {
            if (not field->pure()) SetObjectContext(target, field, AC::NIL);
            auto get_field = (ERR (*)(APTR, std::string &))field->GetValue;
            auto error = get_field(target, Value);
            if (not field->pure()) RestoreObjectContext();
            return error;
         }

         int8_t field_value[sizeof(std::string_view)];
         auto fv = get_field_value(target, *field, field_value);
         APTR data = fv.second;

         if (fv.first != ERR::Okay) return fv.first;

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
                  Value.clear();
                  int v = ((int *)data)[0];
                  while (lookup->Name) {
                     const auto mask = lookup->Value;
                     if (((mask & (mask - 1)) IS 0) and (v & mask)) {
                        Value.append(lookup->Name);
                        Value.push_back('|');
                     }
                     lookup++;
                  }
                  if (not Value.empty()) Value.pop_back(); // Remove trailing pipe
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
            auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), *((double *)data));
            if (ec IS std::errc()) Value.assign(buffer, ptr);
         }
         else if (flags & FD_STRING) {
            if (field->GetValue) Value.assign(*((std::string_view *)data));
            else Value.assign(*((std::string *)data));
         }
         else return ERR::UnrecognisedFieldType;

         return ERR::Okay;
      }
      else return ERR::UnsupportedField;
   }

   inline ERR get(FIELD FieldID, std::string_view &Value) {
      Object *target;
      Value = std::string_view();
      if (auto field = FindField(this, FieldID, &target)) {
         if (not field->readable()) return ERR::NoFieldAccess;

         if (field->Flags & FD_STRING) {
            if (field->GetValue) { // Virtual std::string_view
               SetObjectContext(target, field, AC::NIL);
               auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
               auto error = get_field(target, Value);
               RestoreObjectContext();
               return error;
            }
            else Value = *((std::string *)(((int8_t *)target) + field->Offset)); // Direct std::string

            return ERR::Okay;
         }
         else if ((field->Flags & FD_INT) and (field->Flags & FD_LOOKUP)) {
            int8_t field_value[sizeof(std::string_view)];
            auto fv = get_field_value(target, *field, field_value);
            if (fv.first != ERR::Okay) return fv.first;

            // Reading a lookup field as a string is permissible, we just return the string registered in the lookup table
            if (auto lookup = (FieldDef *)field->Arg) {
               int value = ((int *)fv.second)[0];
               while (lookup->Name) {
                  if (value IS lookup->Value) {
                     Value = std::string_view(lookup->Name);
                     return ERR::Okay;
                  }
                  lookup++;
               }
            }
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
         if (not field->readable()) return ERR::NoFieldAccess;

         int8_t field_value[sizeof(std::string_view)];
         auto fv = get_field_value(target, *field, field_value);
         if (fv.first != ERR::Okay) return fv.first;

         if (field->Flags & FD_POINTER) {
            Value = *((T *)fv.second);
            return ERR::Okay;
         }
         else if (field->Flags & FD_STRUCT) {
            if (field->GetValue) Value = *((T *)fv.second);
            else Value = (T)fv.second;
            return ERR::Okay;
         }
         else if (field->Flags & FD_STRING) {
            using pointee = std::remove_pointer_t<T>;
            using plain_pointee = std::remove_cv_t<pointee>;

            if constexpr (std::is_same_v<plain_pointee, char>) {
               if (field->GetValue) { // Virtual std::string_view
                  auto str = (std::string_view *)fv.second;
                  Value = (T)str->data();
               }
               else { // Direct std::string
                  auto str = (std::string *)fv.second;
                  if constexpr (std::is_const_v<pointee>) Value = (T)str->c_str();
                  else Value = (T)str->data();
               }
            }
            else return ERR::FieldTypeMismatch;

            return ERR::Okay;
         }
         return ERR::FieldTypeMismatch;
      }
      else return ERR::UnsupportedField;
   }

   inline ERR get(FIELD FieldID, Unit &Value) {
      Object *target;
      if (auto field = FindField(this, FieldID, &target)) {
         if (not field->readable()) return ERR::NoFieldAccess;

         if (field->Flags & FD_UNIT) {
            if (field->GetValue) {
               SetObjectContext(target, field, AC::NIL);
               auto get_field = (ERR (*)(APTR, Unit &))field->GetValue;
               auto error = get_field(target, Value);
               RestoreObjectContext();
               return error;
            }
            else {
               Value = *((Unit *)(((int8_t *)target) + field->Offset));
               return ERR::Okay;
            }
         }
         else return ERR::FieldTypeMismatch;
      }
      else return ERR::UnsupportedField;
   }

   template <class T> T get(FIELD FieldID)
   requires pcPointer<T> or std::integral<T> or std::floating_point<T> {
      T result(0);
      get(FieldID, result);
      return result;
   };

   template <class T> T get(FIELD FieldID) requires std::is_same_v<T, std::string_view> {
      T result;
      get(FieldID, result);
      return result;
   };

   template <class T> T get(FIELD FieldID) requires std::is_same_v<T, std::string> {
      // Delegates to the std::string getter so that numeric, flag and unit conversions behave identically to
      // get(FIELD, std::string &).  The string_view getter is unsuitable here as it only supports string fields.
      std::string result;
      get(FieldID, result);
      return result;
   };

   template <class T> T get(FIELD FieldID) requires std::is_enum_v<T> {
      std::underlying_type_t<T> result{};
      get(FieldID, result);
      return T(result);
   };

   template <class T> ERR get(FIELD FieldID, std::span<T> &Value, bool TypeCheck = true) {
      Object *target;
      Value = std::span<T>();
      if (auto field = FindField(this, FieldID, &target)) {
         if ((not field->readable()) or (not (field->Flags & FD_ARRAY))) return ERR::NoFieldAccess;

         if ((TypeCheck) and (not (field->Flags & FIELD_TYPECHECK<T>()))) return ERR::FieldTypeMismatch;

         if (field->GetValue) {
            if (not field->pure()) SetObjectContext(target, field, AC::NIL);
            auto get_field = (ERR (*)(APTR, std::span<T> &))field->GetValue;
            auto error = get_field(target, Value);
            if (not field->pure()) RestoreObjectContext();
            return error;
         }
         else if (field->Flags & FD_CPP) { // Embedded kt::vector<T>
            auto vec = (kt::vector<T> *)(((int8_t *)target) + field->Offset);
            Value = std::span<T>(vec->data(), vec->size());
            return ERR::Okay;
         }
         else if (field->Arg) { // Fixed-size embedded array
            Value = std::span<T>((T *)(((int8_t *)target) + field->Offset), size_t(field->Arg));
            return ERR::Okay;
         }
         else return ERR::FieldTypeMismatch;
      }
      else return ERR::UnsupportedField;
   }
};

namespace kt {

inline ERR write_field_value(OBJECTPTR Target, const struct Field *FieldPtr, const FieldValue &Value) {
   if ((Value.Type & FD_STRING) and (Value.Type & FD_CPP)) {
      return FieldPtr->WriteValue(Target, FieldPtr, Value.Type, &Value.CPPString);
   }
   else if (Value.Type & FD_ARRAY) {
      return FieldPtr->WriteValue(Target, FieldPtr, Value.Type, &Value.Span);
   }
   else if (Value.Type & (FD_POINTER|FD_STRING|FD_FUNCTION|FD_UNIT)) {
      return FieldPtr->WriteValue(Target, FieldPtr, Value.Type, Value.Pointer);
   }
   else if (Value.Type & (FD_DOUBLE|FD_FLOAT)) {
      return FieldPtr->WriteValue(Target, FieldPtr, Value.Type, &Value.Double);
   }
   else if (Value.Type & FD_INT64) {
      return FieldPtr->WriteValue(Target, FieldPtr, Value.Type, &Value.Int64);
   }
   else return FieldPtr->WriteValue(Target, FieldPtr, Value.Type, &Value.Int);
}

// Object creation helper class.  Usage examples:
//
//   objFile::create file { fl::Path(URI), fl::Flags(FL::READ) };
//   if (file.ok()) { ... }

template<class T = Object>
class Create {
   private:
      T *obj;

      inline void freeObject() {
         if (obj) {
            FreeResource(obj->UID);
            obj = nullptr;
         }
      }

   public:
      ERR error;

      // Return an unscoped direct object pointer.  NB: Globals are still tracked to their owner; use untracked() if
      // you don't want this.

      template <typename... Args> static T * global(Args&&... Fields) {
         kt::Create<T> object({ std::forward<Args>(Fields)... });
         if (object.ok()) return object.detach();
         else return nullptr;
      }

      inline static T * global(const std::initializer_list<FieldValue> Fields) {
         kt::Create<T> object(Fields);
         if (object.ok()) return object.detach();
         else return nullptr;
      }

      // Return an unscoped local object (suitable for class allocations only).

      template <typename... Args> static T * local(Args&&... Fields) {
         kt::Create<T> object({ std::forward<Args>(Fields)... }, NF::LOCAL);
         if (object.ok()) return object.detach();
         else return nullptr;
      }

      inline static T * local(const std::initializer_list<FieldValue> Fields) {
         kt::Create<T> object(Fields, NF::LOCAL);
         if (object.ok()) return object.detach();
         else return nullptr;
      }

      // Return an unscoped and untracked object pointer.

      template <typename... Args> static T * untracked(Args&&... Fields) {
         kt::Create<T> object({ std::forward<Args>(Fields)... }, NF::UNTRACKED);
         if (object.ok()) return object.detach();
         else return nullptr;
      }

      inline static T * untracked(const std::initializer_list<FieldValue> Fields) {
         kt::Create<T> object(Fields, NF::UNTRACKED);
         if (object.ok()) return object.detach();
         else return nullptr;
      }

      Create(const Create &) = delete;
      Create & operator=(const Create &) = delete;

      Create(Create &&Other) noexcept : obj(Other.obj), error(Other.error) {
         Other.obj = nullptr;
      }

      Create & operator=(Create &&Other) noexcept {
         if (this != &Other) {
            freeObject();
            obj = Other.obj;
            error = Other.error;
            Other.obj = nullptr;
         }

         return *this;
      }

      // Create a scoped object that is not initialised.

      Create(NF Flags = NF::NIL) : obj(nullptr), error(ERR::NewObject) {
         if (NewObject(T::CLASS_ID, Flags, (Object **)&obj) IS ERR::Okay) {
            error = ERR::Okay;
         }
      }

      // Create a scoped object that is fully initialised.

      Create(const std::initializer_list<FieldValue> Fields, NF Flags = NF::NIL) : obj(nullptr), error(ERR::Failed) {
         kt::Log log("CreateObject");
         log.branch(T::CLASS_NAME);

         if (NewObject(T::CLASS_ID, NF::SUPPRESS_LOG|Flags, (Object **)&obj) IS ERR::Okay) {
            for (auto &f : Fields) {
               OBJECTPTR target;
               if (auto field = FindField(obj, f.FieldID, &target)) {
                  if (not (field->Flags & (FD_WRITE|FD_INIT))) {
                     error = log.warning(ERR::NoFieldAccess);
                     return;
                  }
                  else {
                     if ((error = target->lock()) != ERR::Okay) {
                        error = log.warning(error);
                        return;
                     }

                     error = write_field_value(target, field, f);

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
         freeObject();
      }

      T * operator->() { return obj; }; // Promotes underlying methods and fields
      const T * operator->() const { return obj; };
      T * operator*() { return obj; };
      const T * operator*() const { return obj; };

      [[nodiscard]] inline T * get() { return obj; }
      [[nodiscard]] inline const T * get() const { return obj; }

      [[nodiscard]] inline bool ok() const { return error IS ERR::Okay; }

      [[nodiscard]] inline T * detach() { // Return a direct pointer to the object and prevent automated destruction
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
struct acDragDrop      { static const AC id = AC::DragDrop; OBJECTPTR Source; int Item; std::string_view Datatype; };
struct acDraw          { static const AC id = AC::Draw; int X; int Y; int Width; int Height; };
struct acGetKey        { static const AC id = AC::GetKey; std::string_view Key; std::string *Value; };
struct acMove          { static const AC id = AC::Move; double DeltaX; double DeltaY; double DeltaZ; };
struct acMoveToPoint   { static const AC id = AC::MoveToPoint; double X; double Y; double Z; MTF Flags; };
struct acNewChild      { static const AC id = AC::NewChild; OBJECTPTR Object; };
struct acNewOwner      { static const AC id = AC::NewOwner; OBJECTPTR NewOwner; };
struct acRead          { static const AC id = AC::Read; std::span<int8_t> Buffer; int Result; };
struct acRedimension   { static const AC id = AC::Redimension; double X; double Y; double Z; double Width; double Height; double Depth; };
struct acRedo          { static const AC id = AC::Redo; int Steps; };
struct acRename        { static const AC id = AC::Rename; std::string_view Name; };
struct acResize        { static const AC id = AC::Resize; double Width; double Height; double Depth; };
struct acSaveImage     { static const AC id = AC::SaveImage; OBJECTPTR Dest; union { CLASSID ClassID; CLASSID Class; }; };
struct acSaveToObject  { static const AC id = AC::SaveToObject; OBJECTPTR Dest; union { CLASSID ClassID; CLASSID Class; }; };
struct acSeek          { static const AC id = AC::Seek; double Offset; SEEK Position; };
struct acSetKey        { static const AC id = AC::SetKey; std::string_view Key; std::string_view Value; };
struct acUndo          { static const AC id = AC::Undo; int Steps; };
struct acWrite         { static const AC id = AC::Write; std::span<const int8_t> Buffer; int Result; };

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

inline ERR acDragDrop(OBJECTPTR Object, OBJECTPTR Source, int Item, std::string_view Datatype) {
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

inline ERR acGetKey(OBJECTPTR Object, std::string_view Key, std::string &Value) {
   struct acGetKey args = { Key, &Value };
   auto error = Action(AC::GetKey, Object, &args);
   if (error != ERR::Okay) Value.clear();
   return error;
}

inline ERR acMove(OBJECTPTR Object, double X, double Y, double Z) {
   struct acMove args = { X, Y, Z };
   return Action(AC::Move, Object, &args);
}

inline ERR acRead(OBJECTPTR Object, std::span<int8_t> Buffer, int *Read = nullptr) {
   struct acRead read = { Buffer };
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

inline ERR acRename(OBJECTPTR Object, std::string_view Name) {
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

inline ERR acUndo(OBJECTPTR Object, int Steps) {
   struct acUndo args = { Steps };
   return Action(AC::Undo, Object, &args);
}

inline ERR acWrite(OBJECTPTR Object, std::span<const int8_t> Buffer, int *Result = nullptr) {
   struct acWrite write = { Buffer };
   if (auto error = Action(AC::Write, Object, &write); error IS ERR::Okay) {
      if (Result) *Result = write.Result;
      return error;
   }
   else {
      if (Result) *Result = 0;
      return error;
   }
}

inline int acWriteResult(OBJECTPTR Object, std::span<const int8_t> Buffer) {
   struct acWrite write = { Buffer };
   if (Action(AC::Write, Object, &write) IS ERR::Okay) return write.Result;
   else return 0;
}

template <class T> inline ERR acSeekStart(OBJECTPTR Object, T Offset) {
   return acSeek(Object, Offset, SEEK::START);
}

template <class T> inline ERR acSeekEnd(OBJECTPTR Object, T Offset) {
   return acSeek(Object, Offset, SEEK::END);
}

template <class T> inline ERR acSeekCurrent(OBJECTPTR Object, T Offset) {
   return acSeek(Object, Offset, SEEK::CURRENT);
}

inline ERR acSetKey(OBJECTPTR Object, std::string_view Key, std::string_view Value) {
   struct acSetKey args = { Key, Value };
   return Action(AC::SetKey, Object, &args);
}
