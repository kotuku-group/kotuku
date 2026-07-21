#pragma once

// Copyright: Paul Manias © 1996-2026
// Generator: idl-c

#include <kotuku/main.h>

class objScript;

// Script flags

enum class SCF : uint32_t {
   NIL = 0,
   EXIT_ON_ERROR = 0x00000001,
   PROCESS_DOC = 0x00000002,
   LOG_ALL = 0x00000004,
};

DEFINE_ENUM_FLAG_OPERATORS(SCF)

struct ScriptArg { // For use with sc::Exec
   CSTRING Name;
   uint32_t Type;
   union {
      APTR    Address;
      int     Int;
      int64_t Int64;
      double  Double;
   };

   ScriptArg(CSTRING pName, OBJECTPTR pValue, uint32_t pType = FD_OBJECTPTR) : Name(pName), Type(pType), Address((APTR)pValue) { }
   ScriptArg(CSTRING pName, std::string &pValue, uint32_t pType = FD_STRING) : Name(pName), Type(pType), Address((APTR)pValue.data()) { }
   ScriptArg(CSTRING pName, const std::string &pValue, uint32_t pType = FD_STRING) : Name(pName), Type(pType), Address((APTR)pValue.data()) { }
   ScriptArg(CSTRING pName, CSTRING pValue, uint32_t pType = FD_STRING) : Name(pName), Type(pType), Address((APTR)pValue) { }
   ScriptArg(CSTRING pName, APTR pValue, uint32_t pType = FD_PTR) : Name(pName), Type(pType), Address(pValue) { }
   ScriptArg(CSTRING pName, int pValue, uint32_t pType = FD_INT) : Name(pName), Type(pType), Int(pValue) { }
   ScriptArg(CSTRING pName, uint32_t pValue, uint32_t pType = FD_INT) : Name(pName), Type(pType), Int(pValue) { }
   ScriptArg(CSTRING pName, int64_t pValue, uint32_t pType = FD_INT64) : Name(pName), Type(pType), Int64(pValue) { }
   ScriptArg(CSTRING pName, double pValue, uint32_t pType = FD_DOUBLE) : Name(pName), Type(pType), Double(pValue) { }
};

// Script class definition

#define VER_SCRIPT (1.000000)

// Script methods

namespace sc {
struct Exec { std::string_view Procedure; const struct ScriptArg *Args; int TotalArgs; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DerefProcedure { FUNCTION Procedure; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Callback { int64_t ProcedureID; const struct ScriptArg *Args; int TotalArgs; ERR Error; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetProcedureID { std::string_view Procedure; int64_t ProcedureID; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DebugLog { std::string_view Options; std::string *Result; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objScript : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::SCRIPT;
   static constexpr CSTRING CLASS_NAME = "Script";

   using create = kt::Create<objScript>;
   objScript(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   std::string Procedure;              // Specifies a procedure name to be executed.
   std::string CacheFile;              // Compilable script languages can be compiled to a cache file.
   std::string Path;                   // The location of a script file to be loaded.
   std::string ErrorMessage;           // A human readable error string may be declared here following a script execution failure.
   std::string Statement;              // Scripts can be executed from any string passed into this field.
   std::string WorkingPath;            // Defines the script's working path (folder).
   kt::vector<std::string> Results;    // Stores multiple string results for languages that support this feature.
   OBJECTID TargetID;                  // Reference to the default container that new script objects will be initialised to.
   SCF      Flags;                     // Optional flags.
   ERR      Error;                     // If a script fails during execution, an error code may be readable here.
   int      CurrentLine;               // Indicates the current line being executed when in debug mode.
   int      LineOffset;                // For debugging purposes, this value is added to any message referencing a line number.
   public:
   int64_t  ProcedureID;          // For callbacks
   KEYVALUE Vars;                 // Global parameters
   const ScriptArg *ProcArgs;     // Procedure args - applies during Exec
   int      ActivationCount;      // Incremented every time the script is activated.
   int      TotalArgs;            // Total number of ProcArgs
   OBJECTID ScriptOwnerID;

#ifdef KOTUKU_CXX_REUSES_BASE_TAIL_PADDING // Padding for the alignment of derived classes
   uint8_t TailPadding[alignof(APTR) - sizeof(OBJECTID)];
#endif

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, const void *Buffer, int Size) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer, Size };
      return Action(AC::DataFeed, this, &args);
   }
   inline ERR getKey(std::string_view Key, std::string &Value) noexcept {
      struct acGetKey args = { Key, &Value };
      auto error = Action(AC::GetKey, this, &args);
      if (error != ERR::Okay) Value.clear();
      return error;
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR reset() noexcept { return Action(AC::Reset, this, nullptr); }
   inline ERR acSetKey(std::string_view FieldName, std::string_view Value) noexcept {
      struct acSetKey args = { FieldName, Value };
      return Action(AC::SetKey, this, &args);
   }
   inline ERR exec(const std::string_view &Procedure, const struct ScriptArg * Args, int TotalArgs) noexcept {
      struct sc::Exec args = { Procedure, Args, TotalArgs };
      return Action(AC(-1), this, &args);
   }
   inline ERR derefProcedure(FUNCTION Procedure) noexcept {
      struct sc::DerefProcedure args = { Procedure };
      return Action(AC(-2), this, &args);
   }
   inline ERR callback(int64_t ProcedureID, const struct ScriptArg * Args, int TotalArgs, ERR * Error) noexcept {
      struct sc::Callback args = { ProcedureID, Args, TotalArgs, (ERR)0 };
      ERR error = Action(AC(-3), this, &args);
      if (Error) *Error = args.Error;
      return error;
   }
   inline ERR getProcedureID(const std::string_view &Procedure, int64_t * ProcedureID) noexcept {
      struct sc::GetProcedureID args = { Procedure, (int64_t)0 };
      ERR error = Action(AC(-4), this, &args);
      if (ProcedureID) *ProcedureID = args.ProcedureID;
      return error;
   }
   inline ERR debugLog(const std::string_view &Options, std::string &Result) noexcept {
      struct sc::DebugLog args = { Options, &Result };
      ERR error = Action(AC(-5), this, &args);
      return error;
   }

   // Customised field getting

   inline ERR getProcedure(std::string_view &Value) noexcept {
      Value = this->Procedure;
      return ERR::Okay;
   }

   inline ERR getCacheFile(std::string_view &Value) noexcept {
      Value = this->CacheFile;
      return ERR::Okay;
   }

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getErrorMessage(std::string_view &Value) noexcept {
      Value = this->ErrorMessage;
      return ERR::Okay;
   }

   inline ERR getStatement(std::string_view &Value) noexcept {
      Value = this->Statement;
      return ERR::Okay;
   }

   inline ERR getWorkingPath(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getResults(std::span<std::string> &Value) noexcept {
      auto ktv = (kt::vector<std::string> *)(((int8_t *)this) + CLASS_OFFSET + 192);
      Value = std::span<std::string>(ktv->data(), ktv->size());
      return ERR::Okay;
   }

   inline ERR getTarget(OBJECTID &Value) noexcept {
      Value = this->TargetID;
      return ERR::Okay;
   }

   inline ERR getFlags(SCF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getError(ERR &Value) noexcept {
      Value = this->Error;
      return ERR::Okay;
   }

   inline ERR getCurrentLine(int &Value) noexcept {
      Value = this->CurrentLine;
      return ERR::Okay;
   }

   inline ERR getLineOffset(int &Value) noexcept {
      Value = this->LineOffset;
      return ERR::Okay;
   }


   // Customised field setting

   inline ERR setProcedure(const std::string_view &Value) noexcept {
      this->Procedure = Value;
      return ERR::Okay;
   }

   inline ERR setCacheFile(const std::string_view &Value) noexcept {
      this->CacheFile = Value;
      return ERR::Okay;
   }

   inline ERR setPath(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, 0x00804500, &Value);
   }

   inline ERR setErrorMessage(const std::string_view &Value) noexcept {
      this->ErrorMessage = Value;
      return ERR::Okay;
   }

   inline ERR setStatement(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setWorkingPath(const std::string_view &Value) noexcept {
      this->WorkingPath = Value;
      return ERR::Okay;
   }

   inline ERR setResults(const kt::vector<std::string> &Value) noexcept {
      this->Results = Value;
      return ERR::Okay;
   }

   inline ERR setTarget(OBJECTID Value) noexcept {
      this->TargetID = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const SCF Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setLineOffset(const int Value) noexcept {
      this->LineOffset = Value;
      return ERR::Okay;
   }

};

namespace sc {
inline ERR Call(const FUNCTION &Function, std::span<const ScriptArg> Args) noexcept {
   struct Callback args = { Function.ProcedureID, Args.data(), int(Args.size()), ERR::Okay };
   return Action(sc::Callback::id, Function.Context, &args);
}

inline ERR Call(const FUNCTION &Function, std::span<const ScriptArg> Args, ERR &Result) noexcept {
   struct Callback args = { Function.ProcedureID, Args.data(), int(Args.size()), ERR::Okay };
   ERR error = Action(sc::Callback::id, Function.Context, &args);
   Result = args.Error;
   return(error);
}

template <std::size_t SIZE> ERR Call(const FUNCTION &Function, const std::array<ScriptArg, SIZE> &Args) noexcept {
   return Call(Function, std::span<const ScriptArg>(Args));
}

template <std::size_t SIZE> ERR Call(const FUNCTION &Function, const std::array<ScriptArg, SIZE> &Args, ERR &Result) noexcept {
   return Call(Function, std::span<const ScriptArg>(Args), Result);
}

inline ERR Call(const FUNCTION &Function) noexcept {
   return Call(Function, std::span<const ScriptArg>());
}

inline ERR Call(const FUNCTION &Function, ERR &Result) noexcept {
   return Call(Function, std::span<const ScriptArg>(), Result);
}
} // namespace
