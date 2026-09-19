// Load and dump code.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#include <kotuku/main.h>
#include <kotuku/modules/tiri.h>
#include <errno.h>
#include <stdio.h>
#include <string>
#include <utility>

#define lj_load_c
#define LUA_CORE

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_func.h"
#include "lj_frame.h"
#include "lj_vm.h"
#include "../parser/lexer.h"
#include "../parser/parser_diagnostics.h"
#include "lj_bcdump.h"
#include "../parser/parser.h"
#include "../../defs.h"
#include "load.h"

//********************************************************************************************************************
// Load Lua source code and bytecode

static TValue * cpparser(lua_State *L, lua_CFunction dummy, APTR ud)
{
   auto ls = (LexState *)ud;
   int bc;

   cframe_errfunc(L->cframe) = -1;  //  Inherit error function.
   bc = ls->is_bytecode;
   if (ls->mode and !strchr(ls->mode, bc ? 'b' : 't')) {
      setstrV(L, L->top++, lj_err_str(L, ErrMsg::XMODE));
      lj_err_throw(L, LUA_ERRSYNTAX);
   }
   GCproto *pt = bc ? lj_bcread(ls) : lj_parse(ls);
   GCfunc *fn = lj_func_newL_empty(L, pt, tabref(L->env));
   // Don't combine above/below into one statement.
   setfuncV(L, L->top++, fn);
   return nullptr;
}

//********************************************************************************************************************
// Note: LexState is heap-allocated and manually destroyed because Windows SEH (used by lj_err_throw via
// RaiseException) does not invoke C++ destructors for foreign exceptions under MSVC's default /EHsc mode.
// Stack-allocated C++ objects with non-trivial destructors would leak their internal allocations (bc_stack,
// vstack) when a parse error occurs.

static int load(lua_State *Lua, std::string_view Source, CSTRING SourceName, BytecodeLoadMetadata *Metadata,
   BytecodeLoadPolicy Policy = BytecodeLoadPolicy::Install)
{
   if (Metadata) *Metadata = {};

   if (Lua->parser_diagnostics) {
      delete Lua->parser_diagnostics;
      Lua->parser_diagnostics = nullptr;
   }

   auto *ls = new LexState(Lua, Source, SourceName, std::nullopt);
   ls->bytecode_load_policy = Policy;

   // Set diagnose mode if enabled - this allows lexer to collect errors instead of throwing

   if ((Lua->script->JitOptions & JOF::DIAGNOSE) != JOF::NIL) ls->diagnose_mode = true;
   Lua->script->CapturedVariables.clear();  // Clear previous captures before new parse

   auto status = lj_vm_cpcall(Lua, nullptr, ls, cpparser); // Call the parser

   BytecodeLoadMetadata completed;
   if (not status and Metadata and ls->is_bytecode) {
      completed.ImportedModules = std::move(ls->bytecode_import_module_records);
      completed.Package = std::move(ls->bytecode_package_identity);
      completed.Operations = ls->bytecode_load_operations;
      completed.Bytecode = true;
   }

   // Cleanup any pending import lexers left behind if parsing was interrupted by SEH
   for (void *lex : Lua->pending_import_lexers) {
      delete (LexState *)lex;
   }
   Lua->pending_import_lexers.clear();

   delete ls;  // Manual cleanup required - SEH doesn't call C++ destructors
   if (not status and Metadata) *Metadata = std::move(completed);
   lj_gc_check(Lua);
   return status;
}

extern int lua_load(lua_State *Lua, std::string_view Source, CSTRING SourceName)
{
   return load(Lua, Source, SourceName, nullptr);
}

extern int lj_load_with_bytecode_metadata(
   lua_State *Lua, std::string_view Source, CSTRING SourceName, BytecodeLoadMetadata &Metadata)
{
   return load(Lua, Source, SourceName, &Metadata);
}

extern int lj_validate_bytecode(
   lua_State *Lua, std::string_view Source, CSTRING SourceName, BytecodeLoadMetadata &Metadata)
{
   return load(Lua, Source, SourceName, &Metadata, BytecodeLoadPolicy::ValidateOnly);
}

//********************************************************************************************************************
// Dump bytecode to a function writer

extern int lua_dump(lua_State *L, lua_Writer Writer, APTR Data)
{
   cTValue *o = L->top - 1;
   lj_checkapi(L->top > L->base, "top slot empty");
   if (tvisfunc(o) and isluafunc(funcV(o))) return lj_bcwrite(L, funcproto(funcV(o)), Writer, Data, 0);
   else return 1;
}
