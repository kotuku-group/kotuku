// Instruction dispatch handling.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#pragma once

#include "lj_obj.h"
#include "lj_bc.h"
#include "lj_jit.h"

// Type of hot counter. Must match the code in the assembler VM.
// 16 bits are sufficient. Only 0.0015% overhead with maximum slot penalty.
using HotCount = uint16_t;

// Number of hot counter hash table entries (must be a power of two).
constexpr int HOTCOUNT_SIZE = 64;
constexpr int HOTCOUNT_PCMASK = ((HOTCOUNT_SIZE - 1) * sizeof(HotCount));

// Hotcount decrements.
constexpr int HOTCOUNT_LOOP = 2;
constexpr int HOTCOUNT_CALL = 1;

// This solves a circular dependency problem -- bump as needed. Sigh.
constexpr int GG_NUM_ASMFF = 55;

constexpr int GG_LEN_DDISP = (BC__MAX + GG_NUM_ASMFF);
constexpr int GG_LEN_SDISP = BC_FUNCF;
constexpr int GG_LEN_DISP = (GG_LEN_DDISP + GG_LEN_SDISP);

// Global state, main thread and extra fields are allocated together.

struct GG_State {
   lua_State L;            // Main thread.
   global_State g;         // Global state.
#if LJ_TARGET_ARM
   // Make g reachable via K12 encoded DISPATCH-relative addressing.
   uint8_t align1[(16 - sizeof(global_State)) & 15];
#endif
   jit_State J;            //  JIT state.
   HotCount hotcount[HOTCOUNT_SIZE];   //  Hot counters.
#if LJ_TARGET_ARM
   // Ditto for J.
   uint8_t align2[(16 - sizeof(jit_State) - sizeof(HotCount) * HOTCOUNT_SIZE) & 15];
#endif
   ASMFunction dispatch[GG_LEN_DISP]; // Instruction dispatch tables.
   BCIns bcff[GG_NUM_ASMFF];          // Bytecode for ASM fast functions.
};

#define GG_OFS(field) ((int)offsetof(GG_State, field))
#define G2GG(gl)      ((GG_State *)((char *)(gl) - GG_OFS(g)))
#define J2GG(j)       ((GG_State *)((char *)(j) - GG_OFS(J)))
#define L2GG(L)       (G2GG(G(L)))
#define J2G(J)        (&J2GG(J)->g)
#define G2J(gl)       (&G2GG(gl)->J)
#define L2J(L)        (&L2GG(L)->J)

constexpr int GG_G2J = (offsetof(GG_State, J) - offsetof(GG_State, g));
constexpr int GG_G2DISP = (offsetof(GG_State, dispatch) - offsetof(GG_State, g));
constexpr int GG_DISP2G = int(offsetof(GG_State, g)) - int(offsetof(GG_State, dispatch));
constexpr int GG_DISP2J = int(offsetof(GG_State, J)) - int(offsetof(GG_State, dispatch));
constexpr int GG_DISP2HOT = int(offsetof(GG_State, hotcount)) - int(offsetof(GG_State, dispatch));
constexpr int GG_DISP2STATIC = (GG_LEN_DDISP * (int)sizeof(ASMFunction));

#define hotcount_get(gg, pc) (gg)->hotcount[(u32ptr(pc)>>2) & (HOTCOUNT_SIZE-1)]
#define hotcount_set(gg, pc, val) (hotcount_get((gg), (pc)) = (HotCount)(val))

// Dispatch table management.
LJ_FUNC void lj_dispatch_init(GG_State* GG);
LJ_FUNC void lj_dispatch_init_hotcount(global_State* g);
LJ_FUNC void lj_dispatch_update(global_State* g);

// Instruction dispatch callback for hooks or when recording.
LJ_FUNCA void lj_dispatch_ins(lua_State* L, const BCIns* pc);
LJ_FUNCA ASMFunction lj_dispatch_call(lua_State* L, const BCIns* pc);
LJ_FUNCA void lj_dispatch_stitch(jit_State* J, const BCIns* pc);

#define ERRNO_SAVE
#define ERRNO_RESTORE
