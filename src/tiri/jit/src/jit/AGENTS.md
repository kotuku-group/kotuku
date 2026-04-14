The functions in the *.dasc files are typically backed by fallback functions in C++, e.g. assert in vm_x64.dasc has associated code in `src/tiri/jit/src/lib/lib_base.cpp` and `src/tiri/jit/src/lj_ffrecord.cpp`

## Windows x64 Register Aliasing

**Critical**: On Windows x64, LuaJIT registers alias with the calling convention registers differently than on POSIX:

|Register|Windows x64|POSIX x64|
|-|-|-|
|CARG1|rcx|rdi|
|CARG2|rdx|rsi|
|CARG3|r8|rdx|
|CARG4|r9|rcx|
|BASE|rdx|rdx|

**The dangerous aliasing**: On Windows x64, `BASE == rdx == CARG2`. On POSIX, `BASE == rdx == CARG3`.

All aliases can be read from lines 16 - 101 of vm_x64.dasc.  At the time of writing, the following aliases are defined:

```
BASE = rdx
if X64WIN
   KBASE     = rdi		// Must be C callee-save.
   PC        = rsi		// Must be C callee-save.
   DISPATCH  = rbx		// Must be C callee-save.
   KBASEd    = edi
   PCd       = esi
   DISPATCHd = ebx
else
   KBASE     = r15		// Must be C callee-save.
   PC        = rbx		// Must be C callee-save.
   DISPATCH  = r14		// Must be C callee-save.
   KBASEd    = r15d
   PCd       = ebx
   DISPATCHd = r14d
endif

RA     = rcx
RAd    = ecx
RAH    = ch
RAL    = cl
RB     = rbp		// Must be rbp (C callee-save).
RBd    = ebp
RC     = rax		// Must be rax.
RCd    = eax
RCW    = ax
RCH    = ah
RCL    = al
OP     = RBd
RD     = RC
RDd    = RCd
RDW    = RCW
RDL    = RCL
TMPR   = r10
TMPRd  = r10d
ITYPE  = r11
ITYPEd = r11d

if X64WIN
   CARG1  =	rcx // x64/WIN64 C call arguments.
   CARG2  =	rdx
   CARG3  =	r8
   CARG4  =	r9
   CARG1d =	ecx
   CARG2d =	edx
   CARG3d =	r8d
   CARG4d =	r9d
else
   CARG1  = rdi // x64/POSIX C call arguments.
   CARG2  = rsi
   CARG3  = rdx
   CARG4  = rcx
   CARG5  = r8
   CARG6  = r9
   CARG1d = edi
   CARG2d = esi
   CARG3d = edx
   CARG4d = ecx
   CARG5d = r8d
   CARG6d = r9d
endif

// Type definitions
type L      = lua_State
type GL     = global_State
type TVALUE = TValue
type GCOBJ  = GCobj
type STR    = GCstr
type TAB    = GCtab
type LFUNC  = GCfuncL
type CFUNC  = GCfuncC
type PROTO  = GCproto
type UPVAL  = GCupval
type NODE   = Node
type NARGS  = int
type TRACE  = GCtrace
type SBUF   = SBuf
type ARRAY  = GCarray
```

### Common Bug Pattern

When calling C functions that take 4 arguments (e.g., `lj_arr_getidx`), setting `CARG2` on Windows will clobber `BASE`:

```asm
// WRONG - Windows x64 bug:
|  mov CARG2, ARRAY:RB        // Sets rdx, CLOBBERS BASE!
|  lea CARG4, [BASE-8]        // BASE is now garbage!

// CORRECT - Use .if X64WIN to reorder:
|.if X64WIN
|  lea CARG4, [BASE-8]        // Compute CARG4 while BASE is valid
|  mov CARG2, ARRAY:RB        // Now safe to clobber BASE
|.else
|  mov CARG2, ARRAY:RB        // On POSIX, CARG2 != BASE
|  lea CARG4, [BASE-8]        // CARG3 == BASE on POSIX, but computed before clobber
|.endif
```

### Double Aliasing Hazard

When a value is stored in a register that also needs to be saved:

```asm
// WRONG - RB contains array, but we try to save BASE to RB:
|  mov RB, BASE               // Saves BASE, but DESTROYS array!
|  mov CARG2, ARRAY:RB        // Now CARG2 gets wrong value!

// CORRECT - Save array first via temporary:
|.if X64WIN
|  mov CARG3, ARRAY:RB        // Save array to r8 temporarily
|  lea CARG4, [BASE-8]        // Compute result address
|  mov RB, BASE               // Now safe to save BASE to rbp
|  mov CARG2, CARG3           // Move array from temp to CARG2
|  mov CARG3d, TMPRd          // Set index
|.endif
```

### Callee-Saved Registers

`RB` (rbp) is callee-saved, making it safe for preserving values across C calls. Use it to save `BASE` before calls that clobber it, then restore after:

```asm
|  mov RB, BASE               // Save before call
|  call extern some_c_function
|  mov BASE, RB               // Restore after call
```

### Debugging Tips

1. Use `jit.off()` in Tiri scripts to isolate interpreter issues from JIT issues
2. Check all call sites for `lj_arr_getidx` and similar 4-argument C functions
3. Trace register usage backwards from the call to find clobber points
4. Look for patterns where `CARG2` is set before other args that need `BASE`
