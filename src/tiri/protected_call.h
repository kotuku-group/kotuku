// Native ownership must remain outside this boundary: Lua errors need not unwind C++ frames on every platform.
#pragma once

#include <algorithm>
#include <array>
#include "lj_obj.h"
#include "lj_vm.h"
#include "lj_err.h"

template<class F> int protected_tiri_call(lua_State *Lua, F &Function)
{
   // Keep the Lua base/upvalue and immediate-scope checkall semantics.  Suspend caller try handlers so they cannot
   // bypass this cleanup boundary.  Nested script calls can install their own handlers in the saved slots.
   const int try_depth = Lua->try_stack.depth;
   const int checkall_depth = Lua->checkall_stack->depth;
   const auto handler = Lua->try_handler_pc;
   std::array<TryFrame, LJ_MAX_TRY_DEPTH> try_frames;
   std::array<CheckallFrame, LJ_MAX_CHECKALL_DEPTH> checkall_frames;
   std::copy_n(Lua->try_stack.frames, try_depth, try_frames.begin());
   std::copy_n(Lua->checkall_stack->frames, checkall_depth, checkall_frames.begin());
   Lua->try_stack.depth = 0;
   Lua->try_handler_pc = nullptr;
   int status = lj_vm_cpcall(Lua, nullptr, &Function, [](lua_State *, lua_CFunction, void *Data) -> TValue * {
      (*(F *)Data)();
      return nullptr;
   });
   std::copy_n(try_frames.begin(), try_depth, Lua->try_stack.frames);
   std::copy_n(checkall_frames.begin(), checkall_depth, Lua->checkall_stack->frames);
   Lua->try_stack.depth = try_depth;
   Lua->checkall_stack->depth = checkall_depth;
   Lua->try_handler_pc = handler;
   return status;
}
