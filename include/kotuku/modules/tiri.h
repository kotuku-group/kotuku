#pragma once

// Name:      tiri.h
// Copyright: Paul Manias © 2006-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_TIRI (1)

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

#ifdef KOTUKU_STATIC
#define JUMPTABLE_TIRI [[maybe_unused]] static struct TiriBase *TiriBase = nullptr;
#else
#define JUMPTABLE_TIRI struct TiriBase *TiriBase = nullptr;
#endif

struct TiriBase {
#ifndef KOTUKU_STATIC
   ERR (*_SetVariable)(objScript *Script, CSTRING Name, int Type, ...);
#endif // KOTUKU_STATIC
};

#if !defined(KOTUKU_STATIC) and !defined(PRV_TIRI_MODULE)
extern struct TiriBase *TiriBase;
namespace fl {
template<class... Args> ERR SetVariable(objScript *Script, CSTRING Name, int Type, Args... Tags) { return TiriBase->_SetVariable(Script,Name,Type,Tags...); }
} // namespace
#else
namespace fl {
extern ERR SetVariable(objScript *Script, CSTRING Name, int Type, ...);
} // namespace
#endif // KOTUKU_STATIC

