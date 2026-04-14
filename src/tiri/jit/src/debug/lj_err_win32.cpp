// Error handling.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
//
// Someone in Redmond owes me several days of my life. A lot of this is
// undocumented or just plain wrong on MSDN. Some of it can be gathered
// from 3rd party docs or must be found by trial-and-error. They really
// don't want you to write your own language-specific exception handler
// or to interact gracefully with MSVC. :-(
//
// Apparently MSVC doesn't call C++ destructors for foreign exceptions
// unless you compile your C++ code with /EHa. Unfortunately this means
// catch (...) also catches things like access violations. The use of
// _set_se_translator doesn't really help, because it requires /EHa, too.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define LUA_CORE

#include <windows.h>

#include "lj_err.h"
#include "lj_frame.h"
#include "lj_jit.h"
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "lj_trace.h"

// Taken from: http://www.nynaeve.net/?p=99
typedef struct UndocumentedDispatcherContext {
   ULONG64 ControlPc;
   ULONG64 ImageBase;
   PRUNTIME_FUNCTION FunctionEntry;
   ULONG64 EstablisherFrame;
   ULONG64 TargetIp;
   PCONTEXT ContextRecord;
   void (*LanguageHandler)(void);
   PVOID HandlerData;
   PUNWIND_HISTORY_TABLE HistoryTable;
   ULONG ScopeIndex;
   ULONG Fill0;
} UndocumentedDispatcherContext;

#if LJ_TARGET_X64 && defined(MINGW_SDK_INIT)
// Workaround for broken MinGW64 declaration.
VOID RtlUnwindEx_FIXED(PVOID, PVOID, PVOID, PVOID, PVOID, PVOID) asm("RtlUnwindEx");
#define RtlUnwindEx RtlUnwindEx_FIXED
#endif

#define LJ_MSVC_EXCODE     ((DWORD)0xe06d7363)
#define LJ_GCC_EXCODE      ((DWORD)0x20474343)
#define LJ_EXCODE    ((DWORD)0xe24c4a00)
#define LJ_EXCODE_MAKE(c)  (LJ_EXCODE | (DWORD)(c))
#define LJ_EXCODE_CHECK(cl)   (((cl) ^ LJ_EXCODE) <= 0xff)
#define LJ_EXCODE_ERRCODE(cl) ((int)((cl) & 0xff))

extern void * err_unwind(lua_State *L, void *, int);
extern "C" void setup_try_handler(lua_State *);

// Sentinel value returned by err_unwind when a try-except handler is found.
#define ERR_TRYHANDLER ((void*)(intptr_t)-2)

// Windows exception handler for interpreter frame.  Called from buildvm_peobj.cpp

extern "C" int lj_err_unwind_win(EXCEPTION_RECORD *rec, void* f, CONTEXT* ctx, UndocumentedDispatcherContext* dispatch)
{
   void* cf = f;
   lua_State* L = cframe_L(cf);
   int errcode = LJ_EXCODE_CHECK(rec->ExceptionCode) ? LJ_EXCODE_ERRCODE(rec->ExceptionCode) : LUA_ERRRUN;

   if ((rec->ExceptionFlags & 6)) {  // EH_UNWINDING|EH_EXIT_UNWIND
      // If we're resuming at a try-except handler, skip the normal unwind processing.
      // The state has already been set up by setup_try_handler().
      if (L->try_handler_pc) return 1;  // ExceptionContinueSearch - let RtlUnwindEx continue to target

      // Unwind internal frames.
      err_unwind(L, cf, errcode);
   }
   else {
      void *cf2 = err_unwind(L, cf, 0);
      if (cf2 IS ERR_TRYHANDLER) {
         // A try-except handler was found. check_try_handler() only recorded
         // the handler PC. Now we need to set up the actual state.
         setup_try_handler(L);
         //
         // Resume execution at the handler PC using the VM entry point.
         // Use 'cf' (the current frame) as TargetFrame, matching the pattern
         // used by the standard exception handlers.
         RtlUnwindEx(cf, (void*)lj_vm_resume_try_eh, rec, (void*)(uintptr_t)0, ctx, dispatch->HistoryTable);
         // RtlUnwindEx should never return.
      }
      else if (cf2) {  // We catch it, so start unwinding the upper frames.
         if (rec->ExceptionCode == LJ_MSVC_EXCODE ||
            rec->ExceptionCode == LJ_GCC_EXCODE) {
            setstrV(L, L->top++, lj_err_str(L, ErrMsg::ERRCPP));
         }
         else if (!LJ_EXCODE_CHECK(rec->ExceptionCode)) {
            // Don't catch access violations etc.
            return 1;  //  ExceptionContinueSearch
         }

         // Unwind the stack and call all handlers for all lower C frames
         // (including ourselves) again with EH_UNWINDING set. Then set
         // stack pointer = cf, result = errcode and jump to the specified target.

         RtlUnwindEx(cf, (void*)((cframe_unwind_ff(cf2) and errcode != LUA_YIELD) ?
            lj_vm_unwind_ff_eh :
            lj_vm_unwind_c_eh),
            rec, (void*)(uintptr_t)errcode, ctx, dispatch->HistoryTable);
         // RtlUnwindEx should never return.
      }
   }
   return 1;  //  ExceptionContinueSearch
}

#if LJ_TARGET_X64
 #define CONTEXT_REG_PC  Rip
#elif LJ_TARGET_ARM64
 #define CONTEXT_REG_PC  Pc
#else
 #error "NYI: Windows arch-specific unwinder for JIT-compiled code"
#endif

//********************************************************************************************************************
// Windows unwinder for JIT-compiled code.

extern "C" void err_unwind_win_jit(global_State* g, int errcode)
{
   CONTEXT ctx;
   UNWIND_HISTORY_TABLE hist;

   memset(&hist, 0, sizeof(hist));
   RtlCaptureContext(&ctx);
   while (1) {
      uintptr_t frame, base, addr = ctx.CONTEXT_REG_PC;
      void* hdata;
      PRUNTIME_FUNCTION func = RtlLookupFunctionEntry(addr, &base, &hist);
      if (!func) {  // Found frame without .pdata: must be JIT-compiled code.
         ExitNo exitno;
         uintptr_t stub = lj_trace_unwind(G2J(g), addr - sizeof(MCode), &exitno);
         if (stub) {  // Jump to side exit to unwind the trace.
            ctx.CONTEXT_REG_PC = stub;
            G2J(g)->exitcode = errcode;
            RtlRestoreContext(&ctx, nullptr);  //  Does not return.
         }
         break;
      }
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, addr, func,
         &ctx, &hdata, &frame, nullptr);
      if (!addr) break;
   }
   // Unwinding failed, if we end up here.
}

//********************************************************************************************************************
// Raise Windows exception.

extern "C" void err_raise_ext(global_State *g, int errcode)
{
#if LJ_UNWIND_JIT
   if (tvref(g->jit_base)) {
      err_unwind_win_jit(g, errcode);
      return;  //  Unwinding failed.
   }
#elif LJ_HASJIT
   // Cannot catch on-trace errors for Windows/x86 SEH. Unwind to interpreter.
   setmref(g->jit_base, nullptr);
#endif
   RaiseException(LJ_EXCODE_MAKE(errcode), 1 /* EH_NONCONTINUABLE */, 0, nullptr);
}

#endif // _WIN32
