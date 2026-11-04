#pragma once

// Copyright: Paul Manias © 1996-2026
// Generator: idl-c

#include <kotuku/main.h>

class objModule;

// Module flags

enum class MOF : uint32_t {
   NIL = 0,
   LINK_LIBRARY = 0x00000001,
   STATIC = 0x00000002,
   SYSTEM_PROBE = 0x00000004,
};

DEFINE_ENUM_FLAG_OPERATORS(MOF)

// Internal options for requesting function tables from modules.

enum class MHF : uint32_t {
   NIL = 0,
   STATIC = 0x00000001,
   STRUCTURE = 0x00000002,
   DEFAULT = 0x00000002,
};

DEFINE_ENUM_FLAG_OPERATORS(MHF)

#ifdef KOTUKU_STATIC
__export void CloseCore(void);
__export ERR OpenCore(struct OpenInfo *, struct CoreBase **);
#else
__export struct ModHeader ModHeader;
#endif

#ifdef MOD_NAME
 #ifdef KOTUKU_STATIC
  #define KOTUKU_MOD(init,close,open,expunge,test,IDL,Structures) static struct ModHeader ModHeader(init, close, open, expunge, test, IDL, Structures, TOSTRING(MOD_NAME), TOSTRING(MOD_NAMESPACE));
 #else
  #define KOTUKU_MOD(init,close,open,expunge,test,IDL,Structures) struct ModHeader ModHeader(init, close, open, expunge, test, IDL, Structures, TOSTRING(MOD_NAME), TOSTRING(MOD_NAMESPACE));
 #endif
 #define MOD_PATH ("modules:" TOSTRING(MOD_NAME))
#else
 #define MOD_NAME nullptr
#endif

using ModClose   = void (*)(OBJECTPTR);
using ModInit    = ERR (*)(OBJECTPTR, struct CoreBase*);
using ModOpen    = ERR (*)(OBJECTPTR);
using ModExpunge = ERR (*)(void);
using ModTest    = void (*)(std::string_view, int *, int *);

struct Function {
   APTR    Address;                      // Pointer to the function entry point
   CSTRING Name;                         // Name of the function
   const struct FunctionField * Args;    // A list of parameters accepted by the function
};

struct ModHeader {
   MHF     Flags;                                    // Special flags, type of function table wanted from the Core
   int     CoreTimestamp;                            // Core build date (YYYYMMDD) that this module was compiled against.
   CSTRING Definitions;                              // Module definition string, usable by run-time languages such as Tiri
   ERR (*Init)(OBJECTPTR, struct CoreBase *);        // A one-off initialisation routine for when the module is first opened.
   void (*Close)(OBJECTPTR);                         // A function that will be called each time the module is closed.
   ERR (*Open)(OBJECTPTR);                           // A function that will be called each time the module is opened.
   ERR (*Expunge)(void);                             // Reference to an expunge function to terminate the module.
   void (*Test)(std::string_view, int *, int *);     // A function that can run embedded unit tests in development builds.
   CSTRING Name;                                     // Name of the module
   CSTRING Namespace;                                // A reserved system-wide namespace for function names.
   typedef const std::vector<std::pair<std::string, StructInfo>> STRUCTS;
   STRUCTS *StructDefs;
   class objRootModule *Root;

   ModHeader(ModInit pInit, ModClose pClose, ModOpen pOpen, ModExpunge pExpunge, ModTest pTest,
      CSTRING pDef, STRUCTS *pStructs, CSTRING pName, CSTRING pNamespace) {
      Flags         = MHF::DEFAULT;
      CoreTimestamp = CORE_BUILD_DATE;
      Definitions   = pDef;
      StructDefs    = pStructs;
      Init          = pInit;
      Close         = pClose;
      Open          = pOpen;
      Expunge       = pExpunge;
      Test          = pTest;
      Name          = pName;
      Namespace     = pNamespace;
      Root          = nullptr;
   }
};

// Module class definition

#define VER_MODULE (1.000000)

// Module methods

namespace mod {
struct ResolveSymbol { std::string_view Name; APTR Address; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Test { std::string_view Options; int Passed; int Total; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objModule : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::MODULE;
   static constexpr CSTRING CLASS_NAME = "Module";

   using create = kt::Create<objModule>;
   objModule(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   const struct Function * FunctionList;    // Refers to a list of public functions exported by the module.
   APTR ModBase;                            // The Module's function base (jump table) must be read from this field.
   objRootModule * Root;                    // For internal use only.
   struct ModHeader * Header;               // For internal usage only.
   std::string Name;                        // The name of the module.
   MOF  Flags;                              // Optional flags.
   public:
   static ERR load(std::string Name, OBJECTPTR *Module = nullptr, APTR Functions = nullptr) {
      if (auto module = objModule::create::global(kt::FieldValue(kt::strhash("name"), std::string_view(Name)))) {
         #ifdef KOTUKU_STATIC
            if (Module) *Module = module;
            if (Functions) ((APTR *)Functions)[0] = nullptr;
            return ERR::Okay;
         #else
            APTR functionbase;
            if (!module->getModBase(functionbase)) {
               if (Module) *Module = module;
               if (Functions) ((APTR *)Functions)[0] = functionbase;
               return ERR::Okay;
            }
            else return ERR::GetField;
         #endif
      }
      else return ERR::CreateObject;
   }

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
   inline ERR resolveSymbol(const std::string_view &Name, APTR * Address) noexcept {
      struct mod::ResolveSymbol args = { Name, (APTR)0 };
      ERR error = Action(AC(-1), this, &args);
      if (Address) *Address = args.Address;
      return error;
   }
   inline ERR test(const std::string_view &Options, int * Passed, int * Total) noexcept {
      struct mod::Test args = { Options, (int)0, (int)0 };
      ERR error = Action(AC(-2), this, &args);
      if (Passed) *Passed = args.Passed;
      if (Total) *Total = args.Total;
      return error;
   }

   // Customised field getting

   inline ERR getFunctionList(const struct Function * &Value) noexcept {
      Value = this->FunctionList;
      return ERR::Okay;
   }

   inline ERR getModBase(APTR &Value) noexcept {
      Value = this->ModBase;
      return ERR::Okay;
   }

   inline ERR getName(std::string_view &Value) noexcept {
      Value = this->Name;
      return ERR::Okay;
   }

   inline ERR getFlags(MOF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getDefs(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      return get_field(this, Value);
   }


   // Customised field setting

   inline ERR setFunctionList(const struct Function * Value) noexcept {
      this->FunctionList = Value;
      return ERR::Okay;
   }

   inline ERR setName(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, 0x00804500, &Value);
   }

   inline ERR setFlags(const MOF Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Flags = Value;
      return ERR::Okay;
   }

};