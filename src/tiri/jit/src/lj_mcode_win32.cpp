#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "lj_jit.h"
#include "lj_trace.h"

int MCPROT_RW  = PAGE_READWRITE;
int MCPROT_RX  = PAGE_EXECUTE_READ;
int MCPROT_RWX = PAGE_EXECUTE_READWRITE;

extern "C" void* mcode_alloc_at(jit_State* J, uintptr_t hint, size_t sz, int prot)
{
   void *p = LJ_WIN_VALLOC((void*)hint, sz, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, prot);
   if (!p and !hint) lj_trace_err(J, LJ_TRERR_MCODEAL);
   return p;
}

extern "C" void mcode_free(jit_State* J, void* p, size_t sz)
{
   VirtualFree(p, 0, MEM_RELEASE);
}

extern "C" int mcode_setprot(void* p, size_t sz, int prot)
{
   DWORD oprot;
   return !LJ_WIN_VPROTECT(p, sz, prot, &oprot);
}

#endif // _WIN32
