#pragma once

//  types.h
//
//  (C) Copyright 1996-2023 Paul Manias

#include <type_traits>
#include <utility>
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
   NIL=0,
   STD_C=1,
   SCRIPT=2
};

struct FUNCTION {
   CALL Type;
   uint8_t PadA;
   uint16_t ID; // Unused.  Unique identifier for the function.
   OBJECTPTR Context; // The context at the time the function was created, or a Script reference
   union {
      void * Meta;    // Additional meta data provided by the client.
      int64_t MetaValue;
   };
   union {
      void * Routine;    // CALL::STD_C: Pointer to a C routine
      int64_t ProcedureID; // CALL::SCRIPT: Function identifier, usually a hash
   };

   FUNCTION() : Type(CALL::NIL) { }
   FUNCTION(CALL pType) : Type(pType) { }

   // Script constructor

   FUNCTION(class objScript *pScript, int64_t pProcedure) {
      Type        = CALL::SCRIPT;
      Context     = (OBJECTPTR)pScript;
      ProcedureID = pProcedure;
   }

   // The CALL::STDC constructor is managed by C_FUNCTION() in order to prevent problems with
   // implicit type conversion.

   void clear() { Type = CALL::NIL; }
   bool isC() const { return Type IS CALL::STD_C; }
   bool isScript() const { return Type IS CALL::SCRIPT; }
   bool defined() const { return Type != CALL::NIL; }
};

inline bool operator==(const struct FUNCTION &A, const struct FUNCTION &B)
{
   if (A.Type == CALL::STD_C) return (A.Type == B.Type) and (A.Context == B.Context) and (A.Routine == B.Routine);
   else if (A.Type == CALL::SCRIPT) return (A.Type == B.Type) and (A.Context == B.Context) and (A.ProcedureID == B.ProcedureID);
   else return (A.Type == B.Type);
}
