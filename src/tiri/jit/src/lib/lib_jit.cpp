// JIT library.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#define lib_jit_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_state.h"
#include "lj_bc.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_ircall.h"
#include "lj_iropt.h"
#include "lj_target.h"
#include "lj_trace.h"
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "lj_vmevent.h"
#include "lib.h"

#include "luajit.h"

// jit.* functions

#define LJLIB_MODULE_jit // For lj_libdef.h

//********************************************************************************************************************
// Configure JIT mode for the engine or a specific function/prototype.

static int setjitmode(lua_State* L, int mode)
{
   int idx = 0;
   if (L->base IS L->top or tvisnil(L->base)) {  // jit.on/off/flush([nil])
      mode |= LUAJIT_MODE_ENGINE;
   }
   else {
      // jit.on/off/flush(func|proto, nil|true|false)
      if (tvisfunc(L->base) or tvisproto(L->base)) idx = 1;
      else if (!tvistrue(L->base))  //  jit.on/off/flush(true, nil|true|false)
         goto err;
      if (L->base + 1 < L->top and tvisbool(L->base + 1)) mode |= boolV(L->base + 1) ? LUAJIT_MODE_ALLFUNC : LUAJIT_MODE_ALLSUBFUNC;
      else mode |= LUAJIT_MODE_FUNC;
   }

   if (luaJIT_setmode(L, idx, mode) != 1) {
      if ((mode & LUAJIT_MODE_MASK) IS LUAJIT_MODE_ENGINE) lj_err_caller(L, ErrMsg::NOJIT);
   err:
      lj_err_argt(L, 1, LUA_TFUNCTION);
   }
   return 0;
}

//********************************************************************************************************************
// Enable JIT compilation globally or for a specific function or prototype.

LJLIB_CF(jit_on)
{
   return setjitmode(L, LUAJIT_MODE_ON);
}

//********************************************************************************************************************
// Disable JIT compilation globally or for a specific function or prototype.

LJLIB_CF(jit_off)
{
   return setjitmode(L, LUAJIT_MODE_OFF);
}

//********************************************************************************************************************
// Flush compiled traces for a specific trace or scope, optionally changing JIT mode.

LJLIB_CF(jit_flush)
{
   if (L->base < L->top and tvisnumber(L->base)) {
      int traceno = lj_lib_checkint(L, 1);
      luaJIT_setmode(L, traceno, LUAJIT_MODE_FLUSH | LUAJIT_MODE_TRACE);
      return 0;
   }
   return setjitmode(L, LUAJIT_MODE_FLUSH);
}

//********************************************************************************************************************
// Set boolean key-values in a table for each flag name, based on whether the corresponding bit is set.

static void flagbits_to_table(lua_State* L, GCtab* t, uint32_t flags, uint32_t base, CSTRING str)
{
   for (; *str; base <<= 1, str += 1 + *str) {
      GCstr* name = lj_str_new(L, str + 1, *(uint8_t*)str);
      setboolV(lj_tab_setstr(L, t, name), (flags & base) ? 1 : 0);
      lj_gc_anybarriert(L, t);
   }
}

//********************************************************************************************************************
// Report whether JIT is enabled and return a table of active CPU and optimisation flags as boolean key-values.

LJLIB_CF(jit_status)
{
   jit_State* J = L2J(L);
   L->top = L->base;
   setboolV(L->top++, (J->flags & JIT_F_ON) ? 1 : 0);
   lua_createtable(L, 0, 16);
   GCtab* t = tabV(L->top - 1);
   flagbits_to_table(L, t, J->flags, JIT_F_CPU, JIT_F_CPUSTRING);
   flagbits_to_table(L, t, J->flags, JIT_F_OPT, JIT_F_OPTSTRING);
   return 2;
}

//********************************************************************************************************************
// Attach or detach a VM event callback for JIT-related events such as trace recording.

LJLIB_CF(jit_attach)
{
   GCfunc* fn = lj_lib_checkfunc(L, 1);
   GCstr* s = lj_lib_optstr(L, 2);
   luaL_findtable(L, LUA_REGISTRYINDEX, LJ_VMEVENTS_REGKEY, LJ_VMEVENTS_HSIZE);
   if (s) {  // Attach to given event.
      const uint8_t* p = (const uint8_t*)strdata(s);
      uint32_t h = s->len;
      while (*p) h = h ^ (lj_rol(h, 6) + *p++);
      lua_pushvalue(L, 1);
      lua_rawseti(L, -2, VMEVENT_HASHIDX(h));
      G(L)->vmevmask = VMEVENT_NOCACHE;  //  Invalidate cache.
   }
   else {  // Detach if no event given.
      setnilV(L->top++);
      while (lua_next(L, -2)) {
         L->top--;
         if (tvisfunc(L->top) and funcV(L->top) IS fn) {
            setnilV(lj_tab_set(L, tabV(L->top - 2), L->top - 1));
         }
      }
   }
   return 0;
}

#include "lj_libdef.h" // Includes the LJLIB_MODULE_jit table: lj_lib_cf_jit and lj_lib_init_jit

//********************************************************************************************************************
// jit.util.* functions

#define LJLIB_MODULE_jit_util // For lj_libdef.h

// Reflection API for Lua functions

//********************************************************************************************************************
// Return the Lua prototype for the first argument, optionally allowing non-Lua functions when requested.

static GCproto* check_Lproto(lua_State* L, int nolua)
{
   TValue* o = L->base;
   if (L->top > o) {
      if (tvisproto(o)) return protoV(o);
      else if (tvisfunc(o)) {
         if (isluafunc(funcV(o))) return funcproto(funcV(o));
         else if (nolua) return nullptr;
      }
   }
   lj_err_argt(L, 1, LUA_TFUNCTION);
   return nullptr;  //  unreachable
}

//********************************************************************************************************************
// Write an integer field with the given name into the supplied table.

static void setintfield(lua_State* L, GCtab* t, CSTRING name, int32_t val)
{
   setintV(lj_tab_setstr(L, t, lj_str_newz(L, name)), val);
}

//********************************************************************************************************************
// local info = jit.util.funcInfo(func [,pc])
// Return reflective information about a Lua or C function in a table, optionally at a specific bytecode position.

LJLIB_CF(jit_util_funcInfo)
{
   if (auto pt = check_Lproto(L, 1)) {
      BCPOS pc = (BCPOS)lj_lib_optint(L, 2, 0);
      lua_createtable(L, 0, 16);  //  Increment hash size if fields are added.
      GCtab *t = tabV(L->top - 1);
      setintfield(L, t, "lineDefined", pt->firstline.lineNumber());
      setintfield(L, t, "lastLineDefined", pt->firstline.lineNumber() + pt->numline);
      setintfield(L, t, "stackSlots", pt->framesize);
      setintfield(L, t, "params", pt->numparams);
      setintfield(L, t, "byteCodes", (int32_t)pt->sizebc);
      setintfield(L, t, "gcConsts", (int32_t)pt->sizekgc);
      setintfield(L, t, "nConsts", (int32_t)pt->sizekn);
      setintfield(L, t, "upvalues", (int32_t)pt->sizeuv);
      if (pc < pt->sizebc) setintfield(L, t, "currentLine", lj_debug_line(pt, pc).lineNumber());
      lua_pushboolean(L, (pt->flags & PROTO_VARARG));
      lua_setfield(L, -2, "isVarArg");
      lua_pushboolean(L, (pt->flags & PROTO_CHILD));
      lua_setfield(L, -2, "children");
      setstrV(L, L->top++, proto_chunk_name(pt));
      lua_setfield(L, -2, "source");
      lj_debug_pushloc(L, pt, pc);
      lua_setfield(L, -2, "loc");
      setprotoV(L, lj_tab_setstr(L, t, lj_str_newlit(L, "proto")), pt);
   }
   else {
      GCfunc *fn = funcV(L->base);
      GCtab *t;
      lua_createtable(L, 0, 4);  //  Increment hash size if fields are added.
      t = tabV(L->top - 1);
      if (!iscfunc(fn)) setintfield(L, t, "ffid", fn->c.ffid);
      setintptrV(lj_tab_setstr(L, t, lj_str_newlit(L, "addr")), (intptr_t)(void*)fn->c.f);
      setintfield(L, t, "upvalues", fn->c.nupvalues);
   }
   return 1;
}

//********************************************************************************************************************
// local ins, m = jit.util.funcBC(func, pc)
// Return the raw bytecode word and mode for the given function and bytecode position.

LJLIB_CF(jit_util_funcBC)
{
   GCproto *pt = check_Lproto(L, 0);
   BCPOS pc = (BCPOS)lj_lib_checkint(L, 2);
   if (pc < pt->sizebc) {
      BCIns ins = proto_bc(pt)[pc];
      BCOp op = bc_op(ins);
      lj_assertL(op < BC__MAX, "bad bytecode op %d", op);
      setintV(L->top, ins);
      setintV(L->top + 1, lj_bc_mode[op]);
      L->top += 2;
      return 2;
   }
   return 0;
}

//********************************************************************************************************************
// local k = jit.util.funcK(func, idx)
// Return the numeric or GC constant stored at the given index in a function prototype.

LJLIB_CF(jit_util_funcK)
{
   GCproto *pt = check_Lproto(L, 0);
   ptrdiff_t idx = (ptrdiff_t)lj_lib_checkint(L, 2);
   if (idx >= 0) {
      if (idx < (ptrdiff_t)pt->sizekn) {
         copyTV(L, L->top - 1, proto_knumtv(pt, idx));
         return 1;
      }
   }
   else if (~idx < (ptrdiff_t)pt->sizekgc) {
      GCobj *gc = proto_kgc(pt, idx);
      setgcV(L, L->top - 1, gc, ~gc->gch.gct);
      return 1;
   }
   return 0;
}

//********************************************************************************************************************
// local name = jit.util.funcUName(func, idx)
// Return the name of the upvalue at the given index for a function prototype.

LJLIB_CF(jit_util_funcUName)
{
   GCproto *pt = check_Lproto(L, 0);
   auto idx = (uint32_t)lj_lib_checkint(L, 2);
   if (idx < pt->sizeuv) {
      setstrV(L, L->top - 1, lj_str_newz(L, lj_debug_uvname(pt, idx)));
      return 1;
   }
   else return 0;
}

// Reflection API for traces

//********************************************************************************************************************
// Check the trace argument and return the trace object, without throwing for non-existent trace numbers.

static GCtrace * jit_checktrace(lua_State *L)
{
   TraceNo tr = (TraceNo)lj_lib_checkint(L, 1);
   jit_State* J = L2J(L);
   if (tr > 0 and tr < J->sizetrace) return traceref(J, tr);
   return nullptr;
}

//********************************************************************************************************************
// Names of link types. ORDER LJ_TRLINK

static CSTRING const jit_trlinkname[] = {
  "none", "root", "loop", "tail-recursion", "up-recursion", "down-recursion", "interpreter", "return", "stitch"
};

//********************************************************************************************************************
// local info = jit.util.traceInfo(tr)
// Return high-level information about a recorded trace such as size, exits and link type.

LJLIB_CF(jit_util_traceInfo)
{
   GCtrace *T = jit_checktrace(L);
   if (T) {
      GCtab *t;
      lua_createtable(L, 0, 8);  //  Increment hash size if fields are added.
      t = tabV(L->top - 1);
      setintfield(L, t, "nins", (int32_t)T->nins - REF_BIAS - 1);
      setintfield(L, t, "nk", REF_BIAS - (int32_t)T->nk);
      setintfield(L, t, "link", T->link);
      setintfield(L, t, "nexit", T->nsnap);
      setstrV(L, L->top++, lj_str_newz(L, jit_trlinkname[uint32_t(T->linktype)]));
      lua_setfield(L, -2, "linktype");
      // There are many more fields. Add them only when needed.
      return 1;
   }
   else return 0;
}

//********************************************************************************************************************
// local m, ot, op1, op2, prev = jit.util.traceIR(tr, idx)
// Return IR opcode metadata and operands for the specified instruction in a recorded trace.

LJLIB_CF(jit_util_traceIR)
{
   GCtrace* T = jit_checktrace(L);
   IRRef ref = (IRRef)lj_lib_checkint(L, 2) + REF_BIAS;
   if (T and ref >= REF_BIAS and ref < T->nins) {
      IRIns *ir = &T->ir[ref];
      int32_t m = lj_ir_mode[ir->o];
      setintV(L->top - 2, m);
      setintV(L->top - 1, ir->ot);
      setintV(L->top++, (int32_t)ir->op1 - (irm_op1(m) IS IRMref ? REF_BIAS : 0));
      setintV(L->top++, (int32_t)ir->op2 - (irm_op2(m) IS IRMref ? REF_BIAS : 0));
      setintV(L->top++, ir->prev);
      return 5;
   }
   return 0;
}

//********************************************************************************************************************
// local k, t [, slot] = jit.util.traceK(tr, idx)
// Return the IR constant value, its type and optional slot for a trace constant.

LJLIB_CF(jit_util_traceK)
{
   GCtrace *T = jit_checktrace(L);
   IRRef ref = (IRRef)lj_lib_checkint(L, 2) + REF_BIAS;
   if (T and (ref >= T->nk) and (ref < REF_BIAS)) {
      IRIns *ir = &T->ir[ref];
      int32_t slot = -1;
      if (ir->o IS IR_KSLOT) {
         slot = ir->op2;
         ir = &T->ir[ir->op1];
      }

      lj_ir_kvalue(L, L->top - 2, ir);
      setintV(L->top - 1, (int32_t)irt_type(ir->t));
      if (slot IS -1) return 2;
      setintV(L->top++, slot);
      return 3;
   }
   else return 0;
}

//********************************************************************************************************************
// local snap = jit.util.traceSnap(tr, sn)
// Return a snapshot description including reference, slot count and packed slot map entries.

LJLIB_CF(jit_util_traceSnap)
{
   GCtrace *T = jit_checktrace(L);
   SnapNo sn = (SnapNo)lj_lib_checkint(L, 2);
   if (T and sn < T->nsnap) {
      SnapShot *snap = &T->snap[sn];
      SnapEntry *map = &T->snapmap[snap->mapofs];
      MSize n, nent = snap->nent;
      lua_createtable(L, nent + 2, 0);
      GCtab *t = tabV(L->top - 1);
      setintV(lj_tab_setint(L, t, 0), (int32_t)snap->ref - REF_BIAS);
      setintV(lj_tab_setint(L, t, 1), (int32_t)snap->nslots);
      for (n = 0; n < nent; n++) setintV(lj_tab_setint(L, t, (int32_t)(n + 2)), (int32_t)map[n]);
      setintV(lj_tab_setint(L, t, (int32_t)(nent + 2)), (int32_t)SNAP(255, 0, 0));
      return 1;
   }
   return 0;
}

//********************************************************************************************************************
// local mcode, addr, loop = jit.util.traceMC(tr)
// Return the machine code for a trace together with its address and loop entry offset.

LJLIB_CF(jit_util_traceMC)
{
   GCtrace *T = jit_checktrace(L);
   if (T and T->mcode != nullptr) {
      setstrV(L, L->top - 1, lj_str_new(L, (CSTRING)T->mcode, T->szmcode));
      setintptrV(L->top++, (intptr_t)(void*)T->mcode);
      setintV(L->top++, T->mcloop);
      return 3;
   }
   return 0;
}

//********************************************************************************************************************
// local addr = jit.util.traceExitStub([tr,] exitno)
// Return the address of the exit stub for a global or trace-local exit number.

LJLIB_CF(jit_util_traceExitStub)
{
#ifdef EXITSTUBS_PER_GROUP
   auto exitno = (ExitNo)lj_lib_checkint(L, 1);
   jit_State *J = L2J(L);
   if (exitno < EXITSTUBS_PER_GROUP * LJ_MAX_EXITSTUBGR) {
      setintptrV(L->top - 1, (intptr_t)(void*)exitstub_addr(J, exitno));
      return 1;
   }
#else
   if (L->top > L->base + 1) {  // Don't throw for one-argument variant.
      GCtrace *T = jit_checktrace(L);
      auto exitno = (ExitNo)lj_lib_checkint(L, 2);
      ExitNo maxexit = T->root ? T->nsnap + 1 : T->nsnap;
      if (T and T->mcode != nullptr and exitno < maxexit) {
         setintptrV(L->top - 1, (intptr_t)(void*)exitstub_trace_addr(T, exitno));
         return 1;
      }
   }
#endif
   return 0;
}

//********************************************************************************************************************
// local addr = jit.util.irCallAddr(idx)
// Return the C function address associated with the given IR call index.

LJLIB_CF(jit_util_irCallAddr)
{
   auto idx = (uint32_t)lj_lib_checkint(L, 1);
   if (idx < IRCALL__MAX) {
      setintptrV(L->top - 1, (intptr_t)(void*)lj_ir_callinfo[idx].func);
      return 1;
   }
   else return 0;
}

//********************************************************************************************************************

#include "lj_libdef.h" // Includes the LJLIB_MODULE_jit_util table: lj_lib_cf_jit and lj_lib_init_jit_util

//********************************************************************************************************************
// jit.opt module

#define LJLIB_MODULE_jit_opt // For lj_libdef.h

// Parse optimization level.
static int jitopt_level(jit_State* J, CSTRING str)
{
   if (str[0] >= '0' and str[0] <= '9' and str[1] IS '\0') {
      uint32_t flags;
      if (str[0] IS '0') flags = JIT_F_OPT_0;
      else if (str[0] IS '1') flags = JIT_F_OPT_1;
      else if (str[0] IS '2') flags = JIT_F_OPT_2;
      else flags = JIT_F_OPT_3;
      J->flags = (J->flags & ~JIT_F_OPT_MASK) | flags;
      return 1;  //  Ok.
   }
   else return 0;  //  No match.
}

//********************************************************************************************************************
// Parse optimization flag.

static int jitopt_flag(jit_State *J, CSTRING str)
{
   CSTRING lst = JIT_F_OPTSTRING;
   uint32_t opt;
   int set = 1;

   if (str[0] IS '+') str++;
   else if (str[0] IS '-') {
      str++;
      set = 0;
   }
   else if (str[0] IS 'n' and str[1] IS 'o') {
      str += str[2] IS '-' ? 3 : 2;
      set = 0;
   }

   for (opt = JIT_F_OPT; ; opt <<= 1) {
      size_t len = *(const uint8_t*)lst;
      if (len IS 0) break;
      if (strncmp(str, lst + 1, len) IS 0 and str[len] IS '\0') {
         if (set) J->flags |= opt; else J->flags &= ~opt;
         return 1;  //  Ok.
      }
      lst += 1 + len;
   }

   return 0;  //  No match.
}

//********************************************************************************************************************
// Parse optimization parameter.

static int jitopt_param(jit_State* J, CSTRING str)
{
   CSTRING lst = JIT_P_STRING;
   int i;
   for (i = 0; i < JIT_P__MAX; i++) {
      size_t len = *(const uint8_t*)lst;
      lj_assertJ(len != 0, "bad JIT_P_STRING");
      if (strncmp(str, lst + 1, len) IS 0 and str[len] IS '=') {
         int32_t n = 0;
         CSTRING p = &str[len + 1];
         while (*p >= '0' and *p <= '9')
            n = n * 10 + (*p++ - '0');
         if (*p or (int32_t)n < 0) return 0;  // Malformed number.
         if (i IS JIT_P_sizemcode) {  // Adjust to required range here.
#if LJ_TARGET_JUMPRANGE
            int32_t maxkb = ((1 << (LJ_TARGET_JUMPRANGE - 10)) - 64);
#else
            int32_t maxkb = ((1 << (31 - 10)) - 64);
#endif
            n = (n + (LJ_PAGESIZE >> 10) - 1) & ~((LJ_PAGESIZE >> 10) - 1);
            if (n > maxkb) n = maxkb;
         }

         J->param[i] = (int32_t)n;
         if (i IS JIT_P_hotloop) lj_dispatch_init_hotcount(J2G(J));
         return 1;  //  Ok.
      }
      lst += 1 + len;
   }
   return 0;  //  No match.
}

//********************************************************************************************************************
// jit.opt.start(flags...)

LJLIB_CF(jit_opt_start)
{
   jit_State *J = L2J(L);
   auto nargs = (int)(L->top - L->base);
   if (nargs IS 0) {
      J->flags = (J->flags & ~JIT_F_OPT_MASK) | JIT_F_OPT_DEFAULT;
   }
   else {
      for (int i = 1; i <= nargs; i++) {
         auto str = strdata(lj_lib_checkstr(L, i));
         if (!jitopt_level(J, str) and !jitopt_flag(J, str) and !jitopt_param(J, str)) {
            lj_err_callerv(L, ErrMsg::JITOPT, str);
         }
      }
   }
   return 0;
}

//********************************************************************************************************************

#include "lj_libdef.h" // Includes the LJLIB_MODULE_jit_opt table: lj_lib_cf_jit and lj_lib_init_jit_opt

//********************************************************************************************************************
// JIT compiler initialization

// Default values for JIT parameters.

static const int32_t jit_param_default[JIT_P__MAX + 1] = {
#define JIT_PARAMINIT(len, name, value)   (value),
JIT_PARAMDEF(JIT_PARAMINIT)
#undef JIT_PARAMINIT
  0
};

#if LJ_TARGET_ARM && LJ_TARGET_LINUX
#include <sys/utsname.h>
#endif

//********************************************************************************************************************
// Arch-dependent CPU feature detection.

static uint32_t jit_cpudetect(void)
{
   uint32_t flags = 0;
#if LJ_TARGET_X86ORX64

   uint32_t vendor[4];
   uint32_t features[4];
   if (lj_vm_cpuid(0, vendor) and lj_vm_cpuid(1, features)) {
      flags |= ((features[2] >> 0) & 1) * JIT_F_SSE3;
      flags |= ((features[2] >> 19) & 1) * JIT_F_SSE4_1;
      if (vendor[0] >= 7) {
         uint32_t xfeatures[4];
         lj_vm_cpuid(7, xfeatures);
         flags |= ((xfeatures[1] >> 8) & 1) * JIT_F_BMI2;
      }
   }
   // Don't bother checking for SSE2 -- the VM will crash before getting here.

#elif LJ_TARGET_ARM

   int ver = LJ_ARCH_VERSION;  //  Compile-time ARM CPU detection.
#if LJ_TARGET_LINUX
   if (ver < 70) {  // Runtime ARM CPU detection.
      struct utsname ut;
      uname(&ut);
      if (strncmp(ut.machine, "armv", 4) IS 0) {
         if (ut.machine[4] >= '8') ver = 80;
         else if (ut.machine[4] IS '7') ver = 70;
         else if (ut.machine[4] IS '6') ver = 60;
      }
   }
#endif
   flags |= ver >= 70 ? JIT_F_ARMV7 :
      ver >= 61 ? JIT_F_ARMV6T2_ :
      ver >= 60 ? JIT_F_ARMV6_ : 0;
   flags |= LJ_ARCH_HASFPU IS 0 ? 0 : ver >= 70 ? JIT_F_VFPV3 : JIT_F_VFPV2;

#elif LJ_TARGET_ARM64

   // No optional CPU features to detect (for now).

#elif LJ_TARGET_PPC

#if LJ_ARCH_SQRT
   flags |= JIT_F_SQRT;
#endif

#if LJ_ARCH_ROUND
   flags |= JIT_F_ROUND;
#endif

#else
#error "Missing CPU detection for this architecture"
#endif
   return flags;
}

//********************************************************************************************************************
// Initialise the jit library and register util/opt subtables.

#include "lj_proto_registry.h"

extern int luaopen_jit(lua_State* L)
{
   jit_State *J = L2J(L);
   J->flags = jit_cpudetect() | JIT_F_ON | JIT_F_OPT_DEFAULT;
   memcpy(J->param, jit_param_default, sizeof(J->param));
   lj_dispatch_update(G(L));

   LJ_LIB_REG(L, "jit", jit);  // Creates jit table, leaves it on stack

   // Register jit.util as a subtable of jit (avoid lib_create_table's dotted name handling)
   lua_getglobal(L, "jit");  // Get the jit table we just created
   LJ_LIB_REG(L, nullptr, jit_util);  // Create util table without setting in globals
   lua_setfield(L, -2, "util");
   lua_pop(L, 1);

   // Register jit.opt as a subtable of jit

   lua_getglobal(L, "jit");
   LJ_LIB_REG(L, nullptr, jit_opt);
   lua_setfield(L, -2, "opt");
   lua_pop(L, 1);

   // Register jit interface prototypes for compile-time type inference
   reg_iface_prototype("jit", "on", {}, { TiriType::Any, TiriType::Bool });
   reg_iface_prototype("jit", "off", {}, { TiriType::Any, TiriType::Bool });
   reg_iface_prototype("jit", "flush", {}, { TiriType::Any });
   reg_iface_prototype("jit", "status", { TiriType::Bool, TiriType::Table }, {});
   reg_iface_prototype("jit", "attach", {}, { TiriType::Func, TiriType::Str });

   // Register jit.util interface prototypes
   reg_iface_prototype("jit.util", "funcInfo", { TiriType::Table }, { TiriType::Func, TiriType::Num });
   reg_iface_prototype("jit.util", "funcBC", { TiriType::Num, TiriType::Num }, { TiriType::Func, TiriType::Num });
   reg_iface_prototype("jit.util", "funcK", { TiriType::Any }, { TiriType::Func, TiriType::Num });
   reg_iface_prototype("jit.util", "funcUName", { TiriType::Str }, { TiriType::Func, TiriType::Num });
   reg_iface_prototype("jit.util", "traceInfo", { TiriType::Table }, { TiriType::Num });
   reg_iface_prototype("jit.util", "traceIR", { TiriType::Num, TiriType::Num, TiriType::Num, TiriType::Num, TiriType::Num }, { TiriType::Num, TiriType::Num });
   reg_iface_prototype("jit.util", "traceK", { TiriType::Any, TiriType::Num, TiriType::Num }, { TiriType::Num, TiriType::Num });
   reg_iface_prototype("jit.util", "traceSnap", { TiriType::Table }, { TiriType::Num, TiriType::Num });
   reg_iface_prototype("jit.util", "traceMC", { TiriType::Str, TiriType::Num, TiriType::Num }, { TiriType::Num });
   reg_iface_prototype("jit.util", "traceExitStub", { TiriType::Num }, { TiriType::Num, TiriType::Num });
   reg_iface_prototype("jit.util", "irCallAddr", { TiriType::Num }, { TiriType::Num });

   // Register jit.opt interface prototypes
   reg_iface_prototype("jit.opt", "start", {}, { TiriType::Str }, FProtoFlags::Variadic);

   return 1;
}
