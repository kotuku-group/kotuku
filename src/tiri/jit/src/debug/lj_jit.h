/*
** Common definitions for the JIT compiler.
** Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
*/

#pragma once

#include "lj_obj.h"
#include "lj_ir.h"

// -- JIT engine flags ----------------------------------------------------

// General JIT engine flags. 4 bits.
inline constexpr uint32_t JIT_F_ON = 0x00000001;

// CPU-specific JIT engine flags. 12 bits. Flags and strings must match.
inline constexpr uint32_t JIT_F_CPU = 0x00000010;

#if LJ_TARGET_X86ORX64

inline constexpr uint32_t JIT_F_SSE3 = (JIT_F_CPU << 0);
inline constexpr uint32_t JIT_F_SSE4_1 = (JIT_F_CPU << 1);
inline constexpr uint32_t JIT_F_BMI2 = (JIT_F_CPU << 2);

inline constexpr const char* JIT_F_CPUSTRING = "\4SSE3\6SSE4.1\4BMI2";

#elif LJ_TARGET_ARM

inline constexpr uint32_t JIT_F_ARMV6_ = (JIT_F_CPU << 0);
inline constexpr uint32_t JIT_F_ARMV6T2_ = (JIT_F_CPU << 1);
inline constexpr uint32_t JIT_F_ARMV7 = (JIT_F_CPU << 2);
inline constexpr uint32_t JIT_F_ARMV8 = (JIT_F_CPU << 3);
inline constexpr uint32_t JIT_F_VFPV2 = (JIT_F_CPU << 4);
inline constexpr uint32_t JIT_F_VFPV3 = (JIT_F_CPU << 5);

inline constexpr uint32_t JIT_F_ARMV6 = (JIT_F_ARMV6_|JIT_F_ARMV6T2_|JIT_F_ARMV7|JIT_F_ARMV8);
inline constexpr uint32_t JIT_F_ARMV6T2 = (JIT_F_ARMV6T2_|JIT_F_ARMV7|JIT_F_ARMV8);
inline constexpr uint32_t JIT_F_VFP = (JIT_F_VFPV2|JIT_F_VFPV3);

inline constexpr const char* JIT_F_CPUSTRING = "\5ARMv6\7ARMv6T2\5ARMv7\5ARMv8\5VFPv2\5VFPv3";

#elif LJ_TARGET_PPC

inline constexpr uint32_t JIT_F_SQRT = (JIT_F_CPU << 0);
inline constexpr uint32_t JIT_F_ROUND = (JIT_F_CPU << 1);

inline constexpr const char* JIT_F_CPUSTRING = "\4SQRT\5ROUND";

#else

inline constexpr const char* JIT_F_CPUSTRING = "";

#endif

// Optimization flags. 12 bits.
inline constexpr uint32_t JIT_F_OPT = 0x00010000;
inline constexpr uint32_t JIT_F_OPT_MASK = 0x0fff0000;

inline constexpr uint32_t JIT_F_OPT_FOLD = (JIT_F_OPT << 0);
inline constexpr uint32_t JIT_F_OPT_CSE = (JIT_F_OPT << 1);
inline constexpr uint32_t JIT_F_OPT_DCE = (JIT_F_OPT << 2);
inline constexpr uint32_t JIT_F_OPT_FWD = (JIT_F_OPT << 3);
inline constexpr uint32_t JIT_F_OPT_DSE = (JIT_F_OPT << 4);
inline constexpr uint32_t JIT_F_OPT_NARROW = (JIT_F_OPT << 5);
inline constexpr uint32_t JIT_F_OPT_LOOP = (JIT_F_OPT << 6);
inline constexpr uint32_t JIT_F_OPT_ABC = (JIT_F_OPT << 7);
inline constexpr uint32_t JIT_F_OPT_SINK = (JIT_F_OPT << 8);
inline constexpr uint32_t JIT_F_OPT_FUSE = (JIT_F_OPT << 9);

// Optimizations names for -O. Must match the order above.
inline constexpr const char* JIT_F_OPTSTRING =
  "\4fold\3cse\3dce\3fwd\3dse\6narrow\4loop\3abc\4sink\4fuse";

// Optimization levels set a fixed combination of flags.
inline constexpr uint32_t JIT_F_OPT_0 = 0;
inline constexpr uint32_t JIT_F_OPT_1 = (JIT_F_OPT_FOLD|JIT_F_OPT_CSE|JIT_F_OPT_DCE);
inline constexpr uint32_t JIT_F_OPT_2 = (JIT_F_OPT_1|JIT_F_OPT_NARROW|JIT_F_OPT_LOOP);
inline constexpr uint32_t JIT_F_OPT_3 = (JIT_F_OPT_2|
  JIT_F_OPT_FWD|JIT_F_OPT_DSE|JIT_F_OPT_ABC|JIT_F_OPT_SINK|JIT_F_OPT_FUSE);
inline constexpr uint32_t JIT_F_OPT_DEFAULT = JIT_F_OPT_3;

// -- JIT engine parameters -----------------------------------------------

#if LJ_TARGET_WINDOWS || LJ_64
// See: http://blogs.msdn.com/oldnewthing/archive/2003/10/08/55239.aspx
inline constexpr int JIT_P_sizemcode_DEFAULT = 64;
#else
// Could go as low as 4K, but the mmap() overhead would be rather high.
inline constexpr int JIT_P_sizemcode_DEFAULT = 32;
#endif

// Optimization parameters and their defaults. Length is a char in octal!
#define JIT_PARAMDEF(_) \
  _(\010, maxtrace,   1000)   /* Max. # of traces in cache. */ \
  _(\011, maxrecord,   4000)   /* Max. # of recorded IR instructions. */ \
  _(\012, maxirconst,   500)   /* Max. # of IR constants of a trace. */ \
  _(\007, maxside,   100)   /* Max. # of side traces of a root trace. */ \
  _(\007, maxsnap,   500)   /* Max. # of snapshots for a trace. */ \
  _(\011, minstitch,   0)   /* Min. # of IR ins for a stitched trace. */ \
  \
  _(\007, hotloop,   56)   /* # of iter. to detect a hot loop/call. */ \
  _(\007, hotexit,   10)   /* # of taken exits to start a side trace. */ \
  _(\007, tryside,   4)   /* # of attempts to compile a side trace. */ \
  \
  _(\012, instunroll,   4)   /* Max. unroll for instable loops. */ \
  _(\012, loopunroll,   15)   /* Max. unroll for loop ops in side traces. */ \
  _(\012, callunroll,   3)   /* Max. unroll for recursive calls. */ \
  _(\011, recunroll,   2)   /* Min. unroll for true recursion. */ \
  \
  /* Size of each machine code area (in KBytes). */ \
  _(\011, sizemcode,   JIT_P_sizemcode_DEFAULT) \
  /* Max. total size of all machine code areas (in KBytes). */ \
  _(\010, maxmcode,   512) \
  // End of list.

enum {
#define JIT_PARAMENUM(len, name, value)   JIT_P_##name,
JIT_PARAMDEF(JIT_PARAMENUM)
#undef JIT_PARAMENUM
  JIT_P__MAX
};

#define JIT_PARAMSTR(len, name, value)   #len #name
#define JIT_P_STRING   JIT_PARAMDEF(JIT_PARAMSTR)

// -- JIT engine data structures ------------------------------------------

// Trace compiler state.
enum class TraceState : unsigned int {
  IDLE,   //  Trace compiler idle.
  ACTIVE = 0x10,
  RECORD,   //  Bytecode recording active.
  RECORD_1ST,   //  Record 1st instruction, too.
  START,   //  New trace started.
  END,      //  End of trace.
  ASM,      //  Assemble trace.
  ERR      //  Trace aborted with error.
};

// Backward compatibility aliases for TraceState
inline constexpr auto LJ_TRACE_IDLE       = uint32_t(TraceState::IDLE);
inline constexpr auto LJ_TRACE_ACTIVE     = uint32_t(TraceState::ACTIVE);
inline constexpr auto LJ_TRACE_RECORD     = uint32_t(TraceState::RECORD);
inline constexpr auto LJ_TRACE_RECORD_1ST = uint32_t(TraceState::RECORD_1ST);
inline constexpr auto LJ_TRACE_START      = uint32_t(TraceState::START);
inline constexpr auto LJ_TRACE_END        = uint32_t(TraceState::END);
inline constexpr auto LJ_TRACE_ASM        = uint32_t(TraceState::ASM);
inline constexpr auto LJ_TRACE_ERR        = uint32_t(TraceState::ERR);

// Post-processing action.
enum class PostProc : unsigned int {
  NONE,      //  No action.
  FIXCOMP,   //  Fixup comparison and emit pending guard.
  FIXGUARD,   //  Fixup and emit pending guard.
  FIXGUARDSNAP,   //  Fixup and emit pending guard and snapshot.
  FIXBOOL,   //  Fixup boolean result.
  FIXCONST,   //  Fixup constant results.
  FFRETRY   //  Suppress recording of retried fast functions.
};

// Backward compatibility aliases for PostProc
inline constexpr PostProc LJ_POST_NONE = PostProc::NONE;
inline constexpr PostProc LJ_POST_FIXCOMP = PostProc::FIXCOMP;
inline constexpr PostProc LJ_POST_FIXGUARD = PostProc::FIXGUARD;
inline constexpr PostProc LJ_POST_FIXGUARDSNAP = PostProc::FIXGUARDSNAP;
inline constexpr PostProc LJ_POST_FIXBOOL = PostProc::FIXBOOL;
inline constexpr PostProc LJ_POST_FIXCONST = PostProc::FIXCONST;
inline constexpr PostProc LJ_POST_FFRETRY = PostProc::FFRETRY;

// Machine code type.
#if LJ_TARGET_X86ORX64
using MCode = uint8_t;
#else
using MCode = uint32_t;
#endif

// Linked list of MCode areas.
struct MCLink {
  MCode *next;      //  Next area.
  size_t size;      //  Size of current area.
};

// Stack snapshot header.
struct SnapShot {
  uint32_t mapofs;   //  Offset into snapshot map.
  IRRef1 ref;      //  First IR ref for this snapshot.
  uint16_t mcofs;   //  Offset into machine code in MCode units.
  uint8_t nslots;   //  Number of valid slots.
  uint8_t topslot;   //  Maximum frame extent.
  uint8_t nent;      //  Number of compressed entries.
  uint8_t count;   //  Count of taken exits for this snapshot.
};

inline constexpr uint8_t SNAPCOUNT_DONE = 255;   //  Already compiled and linked a side trace.

// Compressed snapshot entry.
using SnapEntry = uint32_t;

inline constexpr uint32_t SNAP_FRAME = 0x010000;   //  Frame slot.
inline constexpr uint32_t SNAP_CONT = 0x020000;   //  Continuation slot.
inline constexpr uint32_t SNAP_NORESTORE = 0x040000;   //  No need to restore slot.
inline constexpr uint32_t SNAP_SOFTFPNUM = 0x080000;   //  Soft-float number.
inline constexpr uint32_t SNAP_KEYINDEX = 0x100000;   //  Traversal key index.
static_assert(SNAP_FRAME IS TREF_FRAME);
static_assert(SNAP_CONT IS TREF_CONT);
static_assert(SNAP_KEYINDEX IS TREF_KEYINDEX);

[[nodiscard]] inline constexpr SnapEntry SNAP(uint32_t slot, uint32_t flags, uint32_t ref) noexcept {
   return ((SnapEntry(slot) << 24) + flags + ref);
}

[[nodiscard]] inline constexpr SnapEntry SNAP_TR(uint32_t slot, uint32_t tr) noexcept {
   return ((SnapEntry(slot) << 24) +
           (tr & (TREF_KEYINDEX|TREF_CONT|TREF_FRAME|TREF_REFMASK)));
}

[[nodiscard]] inline constexpr SnapEntry SNAP_MKFTSZ(uint32_t ftsz) noexcept {
   return SnapEntry(ftsz);
}

[[nodiscard]] inline constexpr uint32_t snap_ref(SnapEntry sn) noexcept {
   return (sn & 0xffff);
}

[[nodiscard]] inline constexpr BCREG snap_slot(SnapEntry sn) noexcept {
   return BCREG(sn >> 24);
}

[[nodiscard]] inline constexpr bool snap_isframe(SnapEntry sn) noexcept {
   return (sn & SNAP_FRAME);
}

[[nodiscard]] inline constexpr SnapEntry snap_setref(SnapEntry sn, uint32_t ref) noexcept {
   return ((sn & (0xffff0000&~SNAP_NORESTORE)) | ref);
}

[[nodiscard]] inline const BCIns *snap_pc(SnapEntry *sn) noexcept
{
  uint64_t pcbase;
  memcpy(&pcbase, sn, sizeof(uint64_t));
  return (const BCIns *)(pcbase >> 8);
}

// Snapshot and exit numbers.
using SnapNo = uint32_t;
using ExitNo = uint32_t;

// Trace number.
using TraceNo = uint32_t;   //  Used to pass around trace numbers.
using TraceNo1 = uint16_t;   //  Stored trace number.

// Type of link. ORDER LJ_TRLINK

enum class TraceLink : uint8_t {
  NONE = 0,  // Incomplete trace. No link, yet.
  ROOT,      // Link to other root trace.
  LOOP,      // Loop to same trace.
  TAILREC,   // Tail-recursion.
  UPREC,     // Up-recursion.
  DOWNREC,   // Down-recursion.
  INTERP,    // Fallback to interpreter.
  RETURN,    // Return to interpreter.
  STITCH     // Trace stitching.
};

// Trace object.
struct GCtrace {
  GCHeader;
  uint16_t nsnap;   //  Number of snapshots.
  IRRef nins;      //  Next IR instruction. Biased with REF_BIAS.
#if LJ_GC64
  uint32_t unused_gc64;
#endif
  GCRef gclist;
  IRIns *ir;      //  IR instructions/constants. Biased with REF_BIAS.
  IRRef nk;      //  Lowest IR constant. Biased with REF_BIAS.
  uint32_t nsnapmap;   //  Number of snapshot map elements.
  SnapShot *snap;   //  Snapshot array.
  SnapEntry *snapmap;   //  Snapshot map.
  GCRef startpt;   //  Starting prototype.
  MRef startpc;      //  Bytecode PC of starting instruction.
  BCIns startins;   //  Original bytecode of starting instruction.
  MSize szmcode;   //  Size of machine code.
  MCode *mcode;      //  Start of machine code.
  MSize mcloop;      //  Offset of loop start in machine code.
  uint16_t nchild;   //  Number of child traces (root trace only).
  uint16_t spadjust;   //  Stack pointer adjustment (offset in bytes).
  TraceNo1 traceno;   //  Trace number.
  TraceNo1 link;   //  Linked trace (or self for loops).
  TraceNo1 root;   //  Root trace of side trace (or 0 for root traces).
  TraceNo1 nextroot;   //  Next root trace for same prototype.
  TraceNo1 nextside;   //  Next side trace of same root trace.
  uint8_t sinktags;   //  Trace has SINK tags.
  uint8_t topslot;   //  Top stack slot already checked to be allocated.
  TraceLink linktype;   //  Type of link.
  uint8_t unused1;
#ifdef LUAJIT_USE_GDBJIT
  void *gdbjit_entry;   //  GDB JIT entry.
#endif
};

#define gco2trace(o)   check_exp((o)->gch.gct == ~LJ_TTRACE, (GCtrace *)(o))
#define traceref(J, n) \
  check_exp((n)>0 && (MSize)(n)<J->sizetrace, (GCtrace *)gcref(J->trace[(n)]))

static_assert(offsetof(GChead, gclist) == offsetof(GCtrace, gclist));

[[nodiscard]] inline MSize snap_nextofs(GCtrace *T, SnapShot *snap) noexcept
{
  if (snap+1 IS &T->snap[T->nsnap])
    return T->nsnapmap;
  else
    return (snap+1)->mapofs;
}

// Round-robin penalty cache for bytecodes leading to aborted traces.
struct HotPenalty {
  MRef pc;      //  Starting bytecode PC.
  uint16_t val;      //  Penalty value, i.e. hotcount start.
  uint16_t reason;   //  Abort reason (really TraceErr).
};

inline constexpr uint32_t PENALTY_SLOTS = 64;   //  Penalty cache slot. Must be a power of 2.
inline constexpr uint32_t PENALTY_MIN = (36*2);   //  Minimum penalty value.
inline constexpr uint32_t PENALTY_MAX = 60000;   //  Maximum penalty value.
inline constexpr uint32_t PENALTY_RNDBITS = 4;   //  # of random bits to add to penalty value.

// Round-robin backpropagation cache for narrowing conversions.
struct BPropEntry {
  IRRef1 key;      //  Key: original reference.
  IRRef1 val;      //  Value: reference after conversion.
  IRRef mode;      //  Mode for this entry (currently IRCONV_*).
};

// Number of slots for the backpropagation cache. Must be a power of 2.
inline constexpr uint32_t BPROP_SLOTS = 16;

// Scalar evolution analysis cache.
struct ScEvEntry {
  MRef pc;        //  Bytecode PC of FORI.
  IRRef1 idx;     //  Index reference.
  IRRef1 start;   //  Constant start reference.
  IRRef1 stop;    //  Constant stop reference.
  IRRef1 step;    //  Constant step reference.
  IRType1 t;      //  Scalar type.
  uint8_t dir;    //  Direction. 1: +, 0: -.
};

// Reverse bytecode map (IRRef -> PC). Only for selected instructions.
struct RBCHashEntry {
  MRef pc;      //  Bytecode PC.
  GCRef pt;     //  Prototype.
  IRRef ref;    //  IR reference.
};

// Number of slots in the reverse bytecode hash table. Must be a power of 2.
inline constexpr uint32_t RBCHASH_SLOTS = 8;

// 128 bit SIMD constants.
enum class KSimd : unsigned int {
  ABS,
  NEG,
  _MAX
};

// Backward compatibility aliases for KSimd
inline constexpr unsigned int LJ_KSIMD_ABS = unsigned(KSimd::ABS);
inline constexpr unsigned int LJ_KSIMD_NEG = unsigned(KSimd::NEG);
inline constexpr unsigned int LJ_KSIMD__MAX = unsigned(KSimd::_MAX);

enum class K64 : unsigned int {
#if LJ_TARGET_X86ORX64
  TOBIT,      //  2^52 + 2^51
  _2P64,      //  2^64
  M2P64,      //  -2^64
  M2P64_31 = unsigned(M2P64),
#endif
  _MAX,
};

// Backward compatibility aliases for K64
#if LJ_TARGET_X86ORX64
inline constexpr unsigned int LJ_K64_TOBIT = unsigned(K64::TOBIT);
inline constexpr unsigned int LJ_K64_2P64 = unsigned(K64::_2P64);
inline constexpr unsigned int LJ_K64_M2P64 = unsigned(K64::M2P64);
inline constexpr unsigned int LJ_K64_M2P64_31 = unsigned(K64::M2P64_31);
#endif
inline constexpr unsigned int LJ_K64__MAX = unsigned(K64::_MAX);

enum class K32 : unsigned int {
#if LJ_TARGET_X86ORX64
  M2P64_31,   //  -2^64 or -2^31
#endif
#if LJ_TARGET_PPC
  _2P52_2P31,   //  2^52 + 2^31
  _2P52,      //  2^52
  _2P31,      //  2^31
#endif
  _MAX
};

// Backward compatibility aliases for K32
#if LJ_TARGET_X86ORX64
inline constexpr unsigned int LJ_K32_M2P64_31 = unsigned(K32::M2P64_31);
#endif
#if LJ_TARGET_PPC
inline constexpr unsigned int LJ_K32_2P52_2P31 = unsigned(K32::_2P52_2P31);
inline constexpr unsigned int LJ_K32_2P52 = unsigned(K32::_2P52);
inline constexpr unsigned int LJ_K32_2P31 = unsigned(K32::_2P31);
#endif
inline constexpr unsigned int LJ_K32__MAX = unsigned(K32::_MAX);

// Get 16 byte aligned pointer to SIMD constant.
template<typename JitState>
[[nodiscard]] inline TValue *LJ_KSIMD(JitState *J, unsigned int n) noexcept {
   return (TValue *)(((intptr_t)&J->ksimd[2*n] + 15) & ~(intptr_t)15);
}

// Set/reset flag to activate the SPLIT pass for the current trace.
// These are no-ops on 64-bit builds; the SPLIT pass is only needed for 32-bit.
template<typename JitState>
inline void lj_needsplit([[maybe_unused]] JitState *J) noexcept {}
template<typename JitState>
inline void lj_resetsplit([[maybe_unused]] JitState *J) noexcept {}

// Fold state is used to fold instructions on-the-fly.
struct FoldState {
  IRIns ins;      // Currently emitted instruction.
  IRIns left[2];  // Instruction referenced by left operand.
  IRIns right[2]; // Instruction referenced by right operand.
};

// JIT compiler state.
struct jit_State {
  GCtrace cur;       //  Current trace.
  GCtrace *curfinal; //  Final address of current trace (set during asm).

  lua_State *L;      //  Current Lua state.
  const BCIns *pc;   //  Current PC.
  GCfunc *fn;        //  Current function.
  GCproto *pt;       //  Current prototype.
  TRef *base;        //  Current frame base, points into J->slots.

  uint32_t flags;    //  JIT engine flags.
  BCREG maxslot;     //  Relative to baseslot.
  BCREG baseslot;    //  Current frame base, offset into J->slots.

  uint8_t mergesnap;   //  Allowed to merge with next snapshot.
  uint8_t needsnap;   //  Need snapshot before recording next bytecode.
  IRType1 guardemit;   //  Accumulated IRT_GUARD for emitted instructions.
  uint8_t bcskip;   //  Number of bytecode instructions to skip.

  FoldState fold;   //  Fold state.

  const BCIns *bc_min;  //  Start of allowed bytecode range for root trace.
  MSize bc_extent;      //  Extent of the range.

  TraceState state;     //  Trace compiler state.

  int32_t instunroll;   //  Unroll counter for instable loops.
  int32_t loopunroll;   //  Unroll counter for loop ops in side traces.
  int32_t tailcalled;   //  Number of successive tailcalls.
  int32_t framedepth;   //  Current frame depth.
  int32_t retdepth;     //  Return frame depth (count of RETF).

  uint32_t k32[unsigned(K32::_MAX)];  //  Common 4 byte constants used by backends.
  TValue ksimd[unsigned(KSimd::_MAX)*2+1];  //  16 byte aligned SIMD constants.
  TValue k64[unsigned(K64::_MAX)];  //  Common 8 byte constants.

  IRIns *irbuf;     //  Temp. IR instruction buffer. Biased with REF_BIAS.
  IRRef irtoplim;   //  Upper limit of instuction buffer (biased).
  IRRef irbotlim;   //  Lower limit of instuction buffer (biased).
  IRRef loopref;    //  Last loop reference or ref of final LOOP (or 0).

  MSize     sizesnap;     //  Size of temp. snapshot buffer.
  SnapShot  *snapbuf;     //  Temp. snapshot buffer.
  SnapEntry *snapmapbuf;  //  Temp. snapshot map buffer.
  MSize     sizesnapmap;  //  Size of temp. snapshot map buffer.

  PostProc postproc;   //  Required post-processing after execution.

  GCRef *trace;      // Array of traces.
  TraceNo freetrace; // Start of scan for next free trace.
  MSize sizetrace;   // Size of trace array.
  IRRef1 ktrace;     // Reference to KGC with GCtrace.

  IRRef1 chain[IR__MAX];  //  IR instruction skip-list chain anchors.
  TRef slot[LJ_MAX_JSLOTS+LJ_STACK_EXTRA];  //  Stack slot map.

  int32_t param[JIT_P__MAX];  //  JIT engine parameters.

  MCode *exitstubgroup[LJ_MAX_EXITSTUBGR];  //  Exit stub group addresses.

  HotPenalty penalty[PENALTY_SLOTS];  //  Penalty slots.
  uint32_t penaltyslot;   //  Round-robin index into penalty slots.

#ifdef LUAJIT_ENABLE_TABLE_BUMP
  RBCHashEntry rbchash[RBCHASH_SLOTS];  //  Reverse bytecode map.
#endif

  BPropEntry bpropcache[BPROP_SLOTS];  //  Backpropagation cache slots.
  uint32_t bpropslot;   //  Round-robin index into bpropcache slots.

  ScEvEntry scev;   //  Scalar evolution analysis cache slots.

  const BCIns *startpc; //  Bytecode PC of starting instruction.
  TraceNo parent;       //  Parent of current side trace (0 for root traces).
  ExitNo exitno;        //  Exit number in parent of current side trace.
  int exitcode;         //  Exit code from unwound trace.

  BCIns *patchpc;     //  PC for pending re-patch.
  BCIns patchins;     //  Instruction for pending re-patch.

  int mcprot;         //  Protection of current mcode area.
  MCode *mcarea;      //  Base of current mcode area.
  MCode *mctop;       //  Top of current mcode area.
  MCode *mcbot;       //  Bottom of current mcode area.
  size_t szmcarea;    //  Size of current mcode area.
  size_t szallmcarea; //  Total size of all allocated mcode areas.

  TValue errinfo;     //  Additional info element for trace errors.
  uint8_t retryrec;   //  Retry recording.
  bool abort_in_progress; //  True while aborting trace recording (skip try handlers)
};

#ifdef LUA_USE_ASSERT
#define lj_assertJ(c, ...)   lj_assertG_(J2G(J), (c), __VA_ARGS__)
#else
#define lj_assertJ(c, ...)   ((void)J)
#endif
