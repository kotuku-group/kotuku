#pragma once

// Name:      tiri.h
// Copyright: Paul Manias © 2006-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_TIRI (1)

class objTiri;

// JIT behaviour options

enum class JOF : uint32_t {
   NIL = 0,
   DIAGNOSE = 0x00000001,
   DUMP_BYTECODE = 0x00000002,
   PROFILE = 0x00000004,
   TOP_TIPS = 0x00000008,
   TIPS = 0x00000010,
   ALL_TIPS = 0x00000020,
   DISABLE_JIT = 0x00000040,
   TRACE_CFG = 0x00000080,
   TRACE_TYPES = 0x00000100,
   TRACE_TOKENS = 0x00000200,
   TRACE_EXPECT = 0x00000400,
   TRACE_BOUNDARY = 0x00000800,
   TRACE_OPERATORS = 0x00001000,
   TRACE_REGISTERS = 0x00002000,
   TRACE_ASSIGNMENTS = 0x00004000,
   TRACE_VALUE_CATEGORY = 0x00008000,
   TRACE = 0x0000ff80,
};

DEFINE_ENUM_FLAG_OPERATORS(JOF)

// Tiri class definition

#define VER_TIRI (1.000000)

class objTiri : public objScript {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::TIRI;
   static constexpr CSTRING CLASS_NAME = "Tiri";

   using create = kt::Create<objTiri>;

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, const void *Buffer, int Size) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer, Size };
      return Action(AC::DataFeed, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR query() noexcept { return Action(AC::Query, this, nullptr); }
   inline ERR saveToObject(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveToObject args = { Dest, { ClassID } };
      return Action(AC::SaveToObject, this, &args);
   }

   // Customised field getting

   inline ERR getJitOptions(JOF &Value) noexcept {
      auto field = &this->Class->Dictionary[22];
      return field->GetValue(this, &Value);
   }

   inline ERR getProcedures(std::span<std::string> &Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::span<std::string> &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setJitOptions(const JOF Value) noexcept {
      auto field = &this->Class->Dictionary[22];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

};

#ifdef KOTUKU_STATIC
#define JUMPTABLE_TIRI [[maybe_unused]] static struct TiriBase *TiriBase = nullptr;
#else
#define JUMPTABLE_TIRI struct TiriBase *TiriBase = nullptr;
#endif

struct TiriBase {
#ifndef KOTUKU_STATIC
   ERR (*_SetVariable)(objTiri *Script, const std::string_view &Name, int Type, ...);
#endif // KOTUKU_STATIC
};

#if !defined(KOTUKU_STATIC) and !defined(PRV_TIRI_MODULE)
extern struct TiriBase *TiriBase;
namespace fl {
template<class... Args> ERR SetVariable(objTiri *Script, const std::string_view &Name, int Type, Args... Tags) { return TiriBase->_SetVariable(Script,Name,Type,Tags...); }
} // namespace
#else
namespace fl {
extern ERR SetVariable(objTiri *Script, const std::string_view &Name, int Type, ...);
} // namespace
#endif // KOTUKU_STATIC

