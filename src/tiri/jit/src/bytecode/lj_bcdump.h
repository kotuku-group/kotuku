/*
** Bytecode dump definitions.
** Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
*/

#pragma once

#include "lj_obj.h"
#include "../parser/lexer.h"

// Bytecode dump format

/*
** dump   = header proto+ 0U
** header = ESC 'L' 'J' versionB flagsU [namelenU nameB*]
** proto  = lengthU pdata
** pdata  = phead signatureB* dependB* bcinsW* uvdataH* kgc* knum* [debugB*]
** phead  = flagsB numparamsB framesizeB numuvB numkgcU numknU numbcU siglenU deplenU
**          [debuglenU [firstlineU numlineU]]
** signature = sigversionB sigflagsB paramcountU resultcountU resultentriesU typeentry*
** typeentry = typeB metaflagsB constraintU
** depend = depversionB depcountU funccountU depentry* funcentry*
** depentry  = namelenU nameB* funcfirstU funccountU
** funcentry = namelenU nameB* moduleU
** kgc    = kgctypeU { ktab | (loU hiU) | (rloU rhiU iloU ihiU) | strB* }
** knum   = intU0 | (loU1 hiU)
** ktab   = flagsU narrayU nhashU karray* khash*
** karray = ktabk
** khash  = ktabk ktabk
** ktabk  = ktabtypeU { intU | (loU hiU) | strB* }
**
** B = 8 bit, H = 16 bit, W = 32 bit, U = ULEB128 of W, U0/U1 = ULEB128 of W+1
*/

// Bytecode dump header.
constexpr uint8_t BCDUMP_HEAD1 = 0x1b;
constexpr uint8_t BCDUMP_HEAD2 = 0x4c;
constexpr uint8_t BCDUMP_HEAD3 = 0x4a;

// If you perform *any* kind of private modifications to the bytecode itself
// or to the dump format, you *must* set BCDUMP_VERSION to 0x80 or higher.

// 0x86 added the per-prototype module dependency descriptor block.  Version 0x87 replaces the compiler-private
// mod['\31dependency'] activation call with BC_MODACT.  Version 0x88 adds BC_BFUNC and makes generated fast-function
// ordering part of the private bytecode ABI.  Version 0x8a adds BC_BMETH runtime method dispatch.  Version 0x8c adds
// BC_TCTX contextual table designation and cuts over to opt-in table context, which changes the meaning of every
// existing contextual call sequence.  Version 0x8d adds materialised temporary context blocks and consuming close
// activation bytecodes.  Version 0x8e adds the canonical regex.new identity used by regex literals.  Version 0x8f
// separates object.create, object.new and object._state callable identities.  Version 0x90 accepts a struct reference
// in struct.size, which shifts the generated fast-function ordering.  Version 0x92 expands private array member
// identities with uint8, uint16, uint32 and uint64.  Version 0x93 gives int8 its own signed array member identity.
// Version 0x96 adds BC_ISIN and BC_ISNIN. Version 0x97 adds rawtype and shifts the generated fast-function ordering.
// Older chunks are rejected rather than retaining compatibility shims.

constexpr uint8_t BCDUMP_VERSION = 0x97;

// Compatibility flags.

constexpr uint8_t BCDUMP_F_BE = 0x01;
constexpr uint8_t BCDUMP_F_STRIP = 0x02;
constexpr uint8_t BCDUMP_F_FFI = 0x04;
constexpr uint8_t BCDUMP_F_FR2 = 0x08;
constexpr uint8_t BCDUMP_F_KNOWN = (BCDUMP_F_FR2*2-1);

// Type codes for the GC constants of a prototype. Plus length for strings.
enum {
   BCDUMP_KGC_CHILD, BCDUMP_KGC_TAB, BCDUMP_KGC_I64, BCDUMP_KGC_U64,
   BCDUMP_KGC_COMPLEX, BCDUMP_KGC_STR
};

// Type codes for the keys/values of a constant table.
enum {
   BCDUMP_KTAB_NIL, BCDUMP_KTAB_FALSE, BCDUMP_KTAB_TRUE,
   BCDUMP_KTAB_INT, BCDUMP_KTAB_NUM, BCDUMP_KTAB_STR
};

// Bytecode reader/writer

LJ_FUNC int lj_bcwrite(lua_State* L, GCproto* pt, lua_Writer writer, void* data, int strip);
LJ_FUNC GCproto* lj_bcread_proto(LexState* ls);
LJ_FUNC GCproto* lj_bcread(LexState* ls);
