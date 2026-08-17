#pragma once

//  types.h
//
//  (C) Copyright 1996-2026 Paul Manias

#include <type_traits>
#include <utility>
#include <bit>
#include <cstddef>
#include <cstdint>

//********************************************************************************************************************

template <class Tag, typename T>
class strong_typedef {
   public:
      // Constructors
      strong_typedef() : val() { }
      constexpr explicit strong_typedef(const T &Value) : val(Value) { }

      // Accessors
      explicit operator T&() noexcept { return val; }
      explicit operator const T&() const noexcept { return val; }

      bool defined() { return val != 0; }

   private:
      T val;
};

struct SCALE : strong_typedef<SCALE, double> {
    // Make constructors available
    using strong_typedef::strong_typedef;
};

//********************************************************************************************************************
// Function structure, typically used for defining callbacks to functions and procedures of any kind (e.g. standard C,
// Tiri).  Use C_FUNCTION(Routine, Meta) to create a standard C function.

enum class CALL : uint8_t {
   NIL    = 0,
   STD_C  = 1,
   SCRIPT = 2
};

struct FUNCTION {
   struct ScriptData {
      uint32_t ProcedureID;
      int32_t  ContextID;
   };

   CALL Type;
   uint8_t Flags;
   uint16_t ID; // Unused.  Unique identifier for the function.
   OBJECTPTR Context; // The context at the time the function was created, or a Script reference.  Must remain defined once initialised
   union {
      void * Meta;    // Additional meta data provided by the client.
      int64_t MetaValue;
   };
   union {
      void *     Routine;     // CALL::STD_C: Pointer to a C routine
      int64_t    ScriptValue; // Retains the callback transport size and alignment.
      ScriptData Script;      // CALL::SCRIPT: Procedure and optional bound context identifiers.
   };

   static constexpr uint8_t CONSUMED = 0x01;

   FUNCTION() : Type(CALL::NIL), Flags(0), ID(0), Context(nullptr), MetaValue(0), Routine(nullptr) { }
   FUNCTION(CALL pType) : Type(pType), Flags(0), ID(0), Context(nullptr), MetaValue(0), Routine(nullptr) { }

   // Script constructors

   FUNCTION(class objScript *pScript, uint32_t pProcedure) :
      FUNCTION(pScript, pProcedure, 0) { }

   FUNCTION(class objScript *pScript, uint32_t pProcedure, int32_t pContext) {
      Type        = CALL::SCRIPT;
      Flags       = 0;
      ID          = 0;
      Context     = (OBJECTPTR)pScript;
      MetaValue   = 0;
      setScript(pProcedure, pContext);
   }

   // The CALL::STDC constructor is managed by C_FUNCTION() in order to prevent problems with
   // implicit type conversion.

   inline void disable() { Type = CALL::NIL; }
   inline void clear() { Type = CALL::NIL; Flags = 0; MetaValue = 0; clearScript(); }
   inline bool isC() const { return Type IS CALL::STD_C; }
   inline bool isScript() const { return Type IS CALL::SCRIPT; }
   inline bool defined() const { return Type != CALL::NIL; }

   [[nodiscard]] static constexpr int64_t packScriptValue(uint32_t ProcedureID, int32_t ContextID) noexcept {
      uint64_t value = uint64_t(ProcedureID) | (uint64_t(uint32_t(ContextID)) << 32);
      return std::bit_cast<int64_t>(value);
   }

   [[nodiscard]] static constexpr uint32_t unpackProcedureID(int64_t ScriptValue) noexcept {
      return uint32_t(std::bit_cast<uint64_t>(ScriptValue) & UINT32_MAX);
   }

   [[nodiscard]] static constexpr int32_t unpackContextID(int64_t ScriptValue) noexcept {
      return std::bit_cast<int32_t>(uint32_t(std::bit_cast<uint64_t>(ScriptValue) >> 32));
   }

   inline void setScript(uint32_t ProcedureID, int32_t ContextID = 0) {
      Script.ProcedureID = ProcedureID;
      Script.ContextID   = ContextID;
   }

   inline void setScriptValue(int64_t Value) {
      setScript(unpackProcedureID(Value), unpackContextID(Value));
   }

   inline void clearScript() { setScript(0, 0); }
   [[nodiscard]] inline uint32_t procedureID() const { return Script.ProcedureID; }
   [[nodiscard]] inline int32_t contextID() const { return Script.ContextID; }
   [[nodiscard]] inline int64_t scriptValue() const { return packScriptValue(procedureID(), contextID()); }

   inline bool releaseIfStale() {
      if (stale()) {
         unpin();
         disable();
         return true;
      }
      else return false;
   }

   // Weak-pin management for stale callback detection; refer to the zombie object contract in objects.h.
   // Defined in modules/core.h once Object is complete.

   void pin();
   void unpin();
   [[nodiscard]] bool stale() const;

   inline bool identical(const FUNCTION &Other) const {
      if (Type IS CALL::STD_C) {
         return (Other.Type IS Type) and (Other.Context IS Context) and (Other.Routine IS Routine) and
            (Other.MetaValue IS MetaValue);
      }
      else if (Type IS CALL::SCRIPT) {
         return (Other.Type IS Type) and (Other.Context IS Context) and (Other.procedureID() IS procedureID()) and
            (Other.contextID() IS contextID()) and (Other.MetaValue IS MetaValue);
      }
      else return (Other.Type IS Type) and (Other.MetaValue IS MetaValue);
   }

   // consume() informs the Script client that the procedure can be released on return
   inline void consume() { if (Type IS CALL::SCRIPT) Flags |= CONSUMED; }
   inline bool consumed() const { return Flags & CONSUMED; }
};

inline bool operator==(const struct FUNCTION &A, const struct FUNCTION &B)
{
   if (A.Type IS CALL::STD_C) return (A.Type IS B.Type) and (A.Context IS B.Context) and (A.Routine IS B.Routine);
   else if (A.Type IS CALL::SCRIPT) return (A.Type IS B.Type) and (A.Context IS B.Context) and
      (A.procedureID() IS B.procedureID()) and (A.contextID() IS B.contextID());
   else return (A.Type IS B.Type);
}

static_assert((sizeof(void *) != 8) or (sizeof(FUNCTION) IS 32));
static_assert((sizeof(void *) != 8) or (alignof(FUNCTION) IS 8));
static_assert((sizeof(void *) != 8) or (offsetof(FUNCTION, Context) IS 8));
static_assert((sizeof(void *) != 8) or (offsetof(FUNCTION, MetaValue) IS 16));
static_assert((sizeof(void *) != 8) or (offsetof(FUNCTION, Routine) IS 24));
static_assert(offsetof(FUNCTION::ScriptData, ProcedureID) IS 0);
static_assert(offsetof(FUNCTION::ScriptData, ContextID) IS 4);
static_assert(sizeof(FUNCTION::ScriptData) IS sizeof(int64_t));
