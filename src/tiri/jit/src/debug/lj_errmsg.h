// VM error messages.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

// This file may be included multiple times with different ERRDEF macros.

// Basic error handling.
ERRDEF(ERRMEM,   "Not enough memory")
ERRDEF(ERRERR,   "Error in error handling")
ERRDEF(ERRCPP,   "C++ exception")

// Allocations.
ERRDEF(STROV,   "String length overflow")
ERRDEF(UDATAOV, "Userdata length overflow")
ERRDEF(STKOV,   "Stack overflow")
ERRDEF(STKOVM,  "Stack overflow (%s)")
ERRDEF(TABOV,   "Table overflow")
// Table indexing.
ERRDEF(NANIDX,   "Table index is NaN")
ERRDEF(NILIDX,   "Table index is nil")
ERRDEF(NEXTIDX,  "Invalid key to " LUA_QL("next"))

// Metamethod resolving.
ERRDEF(BADCALL,  "Attempt to call a %s value")
ERRDEF(BADOPRT,  "Attempt to %s %s " LUA_QS " (a %s value)")
ERRDEF(BADOPRV,  "Attempt to %s a %s value")
ERRDEF(BADCMPT,  "Attempt to compare %s with %s")
ERRDEF(BADCMPV,  "Attempt to compare two %s values")
ERRDEF(GETLOOP,  "Loop in gettable")
ERRDEF(SETLOOP,  "Loop in settable")
ERRDEF(OPCALL,   "call")
ERRDEF(OPINDEX,  "index")
ERRDEF(BADKEY,   "String key not recognised")
ERRDEF(OPARITH,  "Perform arithmetic on")
ERRDEF(OPCAT,    "Concatenate")
ERRDEF(OPLEN,    "Get length of")

// Type checks.
ERRDEF(BADSELF,  "Calling " LUA_QS " on bad self (%s)")
ERRDEF(BADARG,   "Bad argument #%d to " LUA_QS " (%s)")
ERRDEF(BADTYPE,  "%s expected, got %s")
ERRDEF(BADASSIGN,"Type mismatch: cannot assign %s to %s variable")
ERRDEF(BADCLASS, "Object class mismatch: required class %s, got %s")
ERRDEF(BADCLASSID, "Unknown object class (ID: 0x%08x) for field %s")
ERRDEF(BADFIELD, "Field " LUA_QS " does not exist in class %s")
ERRDEF(BADVAL,   "Invalid value")
ERRDEF(NOVAL,    "Value expected")
ERRDEF(NOCORO,   "Coroutine expected")
ERRDEF(NOTABN,   "Nil or table expected")
ERRDEF(NOTABLE,  "Table expected")
ERRDEF(NOARRAY,  "Array expected")
ERRDEF(NOLFUNC,  "Lua function expected")
ERRDEF(NOFUNCL,  "Function or level expected")
ERRDEF(NOSFT,    "String/function/table expected")
ERRDEF(NOPROXY,  "Boolean or proxy expected")
ERRDEF(NOSTRUCT, "Unknown struct name")
ERRDEF(FORINIT,  LUA_QL("for") " initial value must be a number")
ERRDEF(FORLIM,   LUA_QL("for") " limit must be a number")
ERRDEF(FORSTEP,  LUA_QL("for") " step must be a number")

// C API checks.
ERRDEF(NOENV,    "No calling environment")
ERRDEF(CYIELD,   "Attempt to yield across C-call boundary")
ERRDEF(BADLU,    "Bad light userdata pointer")
ERRDEF(NOGCMM,   "Bad action while in __gc metamethod")
#if LJ_TARGET_WINDOWS
ERRDEF(BADFPU,   "Bad FPU precision (use D3DCREATE_FPU_PRESERVE with DirectX)")
#endif

// Standard library function errors.
ERRDEF(ASSERT,   "Assertion failed!")
ERRDEF(PROTMT,   "Cannot change a protected metatable")
ERRDEF(UNPACK,   "Too many results to unpack")
ERRDEF(RDRSTR,   "Reader function must return a string")
ERRDEF(PRTOSTR,   LUA_QL("tostring") " must return a string to " LUA_QL("print"))
ERRDEF(NUMRNG,   "Number out of range")
ERRDEF(IDXRNG,   "Index out of range")
ERRDEF(BASERNG,  "Base out of range")
ERRDEF(LVLRNG,   "Level out of range")
ERRDEF(SLARGRNG, "Table or string expected")
ERRDEF(INVLVL,   "Invalid level")
ERRDEF(INVOPT,   "Invalid option")
ERRDEF(INVOPTM,  "Invalid option " LUA_QS)
ERRDEF(INVFMT,   "Invalid format")
ERRDEF(SETFENV,  LUA_QL("setfenv") " cannot change environment of given object")
ERRDEF(TABINS,   "Wrong number of arguments to " LUA_QL("insert"))
ERRDEF(TABCAT,   "Invalid value (%s) at index %d in table for " LUA_QL("concat"))
ERRDEF(TABSORT,  "Invalid order function for sorting")
ERRDEF(IOCLFL,   "Attempt to use a closed file")
ERRDEF(IOSTDCL,  "Standard file is closed")
ERRDEF(OSUNIQF,  "Unable to generate a unique filename")
ERRDEF(OSDATEF,  "Field " LUA_QS " missing in date table")
ERRDEF(STRDUMP,  "Unable to dump given function")
ERRDEF(STRSLC,   "String slice too long")
ERRDEF(STRPATB,  "Missing " LUA_QL("[") " after " LUA_QL("%f") " in pattern")
ERRDEF(STRPATC,  "Invalid pattern capture")
ERRDEF(STRPATE,  "Malformed pattern (ends with " LUA_QL("%") ")")
ERRDEF(STRPATM,  "Malformed pattern (missing " LUA_QL("]") ")")
ERRDEF(STRPATU,  "Unbalanced pattern")
ERRDEF(STRPATX,  "Pattern too complex")
ERRDEF(STRCAPI,  "Invalid capture index")
ERRDEF(STRCAPN,  "Too many captures")
ERRDEF(STRCAPU,  "Unfinished capture")
ERRDEF(STRFMT,   "Invalid option " LUA_QS " to " LUA_QL("format"))
ERRDEF(STRGSRV,  "Invalid replacement value (a %s)")
ERRDEF(BADMODN,  "Name conflict for module " LUA_QS)
ERRDEF(JITPROT,  "Runtime code generation failed, restricted kernel?")
ERRDEF(NOJIT,    "JIT compiler disabled")
ERRDEF(JITOPT,   "Unknown or malformed optimization flag " LUA_QS)

// Lexer/parser errors.
ERRDEF(XMODE,    "Attempt to load chunk with wrong mode")
ERRDEF(XNEAR,    "%s near " LUA_QS)
ERRDEF(XLINES,   "Chunk has too many lines")
ERRDEF(XLEVELS,  "Chunk has too many syntax levels")
ERRDEF(XNUMBER,  "Malformed number")
ERRDEF(XLSTR,    "Unfinished long string")
ERRDEF(XLCOM,    "Unfinished long comment")
ERRDEF(XSTR,     "Unfinished string")
ERRDEF(XESC,     "Invalid escape sequence")
ERRDEF(XLDELIM,  "Invalid long string delimiter")
ERRDEF(XTOKEN,   LUA_QS " expected")
ERRDEF(XJUMP,    "Control structure too long")
ERRDEF(XSLOTS,   "Function or expression too complex, exceeded LJ_MAX_SLOTS")
ERRDEF(XLIMC,    "Chunk has more than %d local variables")
ERRDEF(XLIMM,    "Main function has more than %d %s")
ERRDEF(XLIMF,    "Function at line %d has more than %d %s")
ERRDEF(XMATCH,   LUA_QS " expected (to close " LUA_QS " at line %d)")
ERRDEF(XFIXUP,   "Function too long for return fixup")
ERRDEF(XPARAM,   "<name> or " LUA_QL("...") " expected")
ERRDEF(XFUNARG,  "Function arguments expected")
ERRDEF(XSYMBOL,  "Unexpected symbol")
ERRDEF(XDOTS,    "Cannot use " LUA_QL("...") " outside a vararg function")
ERRDEF(XSYNTAX,  "Syntax error")
ERRDEF(XFOR,     LUA_QL("=") " or " LUA_QL("in") " expected")
ERRDEF(XBREAK,   "No loop to break")
ERRDEF(XLEFTCOMPOUND,   "Syntax error in left hand expression in compound assignment")
ERRDEF(XRIGHTCOMPOUND,   "Syntax error in right hand expression in compound assignment")
ERRDEF(XNOTASSIGNABLE,   "Syntax error expression not assignable")
ERRDEF(XCONTINUE,  "No loop to continue")
ERRDEF(XBLANKREAD, "Cannot read blank identifier " LUA_QL("_"))
ERRDEF(XUNDEF,     "Undefined variable " LUA_QS)
ERRDEF(XLUNDEF,    "Undefined label " LUA_QS)
ERRDEF(XLDUP,      "Duplicate label " LUA_QS)
ERRDEF(XFSTR_EMPTY, "Empty interpolation in f-string")
ERRDEF(XFSTR_BRACE, "Unclosed brace in f-string interpolation")
ERRDEF(XNEST,       "Try blocks nested too deeply")
ERRDEF(BADLIBRARY,  "Invalid library name; only alpha-numeric names are permitted with max 96 chars.")

// Bytecode reader errors.
ERRDEF(BCFMT,   "Cannot load incompatible bytecode")
ERRDEF(BCBAD,   "Cannot load malformed bytecode")

// Array errors.
ERRDEF(ARROB,   "Array index %d out of bounds (size %d)")
ERRDEF(ARRRO,   "Attempt to modify read-only array")
ERRDEF(ARRTYPE, "Invalid array element type")
ERRDEF(ARRSTR,  "Byte array expected for string extraction")
ERRDEF(ARREXT,  "Cannot grow external or cached string array")

// Object errors.
ERRDEF(OBJFREED, "Object has been freed")

ERRDEF(THUNKEX,  "Thunk threw an exception on resolution")

#undef ERRDEF

/* Detecting unused error messages:
   awk -F, '/^ERRDEF/ { gsub(/ERRDEF./, ""); printf "grep -q ErrMsg::%s *.[ch] || echo %s\n", $1, $1}' lj_errmsg.h | sh
*/
