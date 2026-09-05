// VM error messages.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

// This file may be included multiple times with different ERRDEF macros.
//
// Each entry pairs the message text with the ERR code that best describes it.  The code is stamped into
// L->CaughtError by the raise entry points in lj_err.cpp, so that Tiri `except` clauses can match on a meaningful
// error rather than the generic ERR::Exception.  Entries that are only ever used as substitution fragments for
// another message (OPCALL, OPINDEX, OPARITH, OPCAT, OPLEN, OPCONTAINS) never reach a raise path, so their code is
// nominal.

// Basic error handling.
ERRDEF(ERRMEM,   ERR::NoMemory,       "Not enough memory")
ERRDEF(ERRERR,   ERR::Recursion,      "Error in error handling")
ERRDEF(ERRCPP,   ERR::Exception,      "C++ exception")

// Allocations.
ERRDEF(STROV,    ERR::BufferOverflow, "String length overflow")
ERRDEF(UDATAOV,  ERR::BufferOverflow, "Userdata length overflow")
ERRDEF(STKOV,    ERR::BufferOverflow, "Stack overflow")
ERRDEF(STKOVM,   ERR::BufferOverflow, "Stack overflow (%s)")
ERRDEF(TABOV,    ERR::BufferOverflow, "Table overflow")
// Table indexing.
ERRDEF(NANIDX,   ERR::InvalidValue,   "Table index is NaN")
ERRDEF(NILIDX,   ERR::InvalidValue,   "Table index is nil")
ERRDEF(NEXTIDX,  ERR::InvalidValue,   "Invalid key to " LUA_QL("next"))

// Metamethod resolving.
ERRDEF(BADCALL,  ERR::TypeMismatch,   "Attempt to call a %s value")
ERRDEF(BADOPRT,  ERR::TypeMismatch,   "Attempt to %s %s " LUA_QS " (a %s value)")
ERRDEF(BADOPRV,  ERR::TypeMismatch,   "Attempt to %s a %s value")
ERRDEF(BADCMPT,  ERR::TypeMismatch,   "Attempt to compare %s with %s")
ERRDEF(BADCMPV,  ERR::TypeMismatch,   "Attempt to compare two %s values")
ERRDEF(GETLOOP,  ERR::Loop,           "Loop in gettable")
ERRDEF(SETLOOP,  ERR::Loop,           "Loop in settable")
ERRDEF(OPCALL,   ERR::TypeMismatch,   "call")
ERRDEF(OPINDEX,  ERR::TypeMismatch,   "index")
ERRDEF(BADKEY,   ERR::UnknownProperty, "String key not recognised")
ERRDEF(OPARITH,  ERR::TypeMismatch,   "perform arithmetic on")
ERRDEF(OPCAT,    ERR::TypeMismatch,   "Concatenate")
ERRDEF(OPLEN,    ERR::TypeMismatch,   "Get length of")
ERRDEF(OPCONTAINS, ERR::TypeMismatch, "test membership in")
ERRDEF(LENMM,    ERR::TypeMismatch,   "__len metamethod must return a number")
// Type checks.
ERRDEF(BADSELF,  ERR::Args,           "Calling " LUA_QS " on bad self (%s)")
ERRDEF(BADARG,   ERR::Args,           "Bad argument #%d to " LUA_QS " (%s)")
ERRDEF(BADTYPE,  ERR::TypeMismatch,   "%s expected, got %s")
ERRDEF(BADASSIGN, ERR::TypeMismatch,  "Type mismatch: cannot assign %s to %s variable")
ERRDEF(BADCLASS, ERR::WrongClass,     "Object class mismatch: required class %s, got %s")
ERRDEF(BADCLASSID, ERR::WrongClass,   "Unknown object class (ID: 0x%08x) for field %s")
ERRDEF(BADFIELD, ERR::FieldNotFound,  "Field " LUA_QS " does not exist in class %s")
ERRDEF(BADVAL,   ERR::InvalidValue,   "Invalid value")
ERRDEF(NOVAL,    ERR::InvalidValue,   "Value expected")
ERRDEF(NOCORO,   ERR::TypeMismatch,   "Coroutine expected")
ERRDEF(NOTABN,   ERR::TypeMismatch,   "Nil or table expected")
ERRDEF(NOTABLE,  ERR::TypeMismatch,   "Table expected")
ERRDEF(NOARRAY,  ERR::TypeMismatch,   "Array expected")
ERRDEF(NOLFUNC,  ERR::TypeMismatch,   "Lua function expected")
ERRDEF(NOFUNCL,  ERR::TypeMismatch,   "Function or level expected")
ERRDEF(NOSFT,    ERR::TypeMismatch,   "String/function/table expected")
ERRDEF(NOPROXY,  ERR::TypeMismatch,   "Boolean or proxy expected")
ERRDEF(NOSTRUCT, ERR::NotFound,       "Unknown struct name")
ERRDEF(FORINIT,  ERR::TypeMismatch,   LUA_QL("for") " initial value must be a number")
ERRDEF(FORLIM,   ERR::TypeMismatch,   LUA_QL("for") " limit must be a number")
ERRDEF(FORSTEP,  ERR::TypeMismatch,   LUA_QL("for") " step must be a number")

// C API checks.
ERRDEF(NOENV,    ERR::InvalidState,   "No calling environment")
ERRDEF(CYIELD,   ERR::NotPossible,    "Attempt to yield across C-call boundary")
ERRDEF(BADLU,    ERR::InvalidValue,   "Bad light userdata pointer")
ERRDEF(NOGCMM,   ERR::NotPossible,    "Bad action while in __gc metamethod")
#if LJ_TARGET_WINDOWS
ERRDEF(BADFPU,   ERR::InvalidState,   "Bad FPU precision (use D3DCREATE_FPU_PRESERVE with DirectX)")
#endif

// Standard library function errors.
ERRDEF(ASSERT,   ERR::Failed,         "Assertion failed!")
ERRDEF(PROTMT,   ERR::ReadOnly,       "Cannot change a protected metatable")
ERRDEF(UNPACK,   ERR::BufferOverflow, "Too many results to unpack")
ERRDEF(RDRSTR,   ERR::TypeMismatch,   "Reader function must return a string")
ERRDEF(PRTOSTR,  ERR::TypeMismatch,   LUA_QL("tostring") " must return a string to " LUA_QL("print"))
ERRDEF(NUMRNG,   ERR::OutOfRange,     "Number out of range")
ERRDEF(IDXRNG,   ERR::OutOfRange,     "Index out of range")
ERRDEF(BASERNG,  ERR::OutOfRange,     "Base out of range")
ERRDEF(LVLRNG,   ERR::OutOfRange,     "Level out of range")
ERRDEF(SLARGRNG, ERR::TypeMismatch,   "Table or string expected")
ERRDEF(INVLVL,   ERR::InvalidValue,   "Invalid level")
ERRDEF(INVOPT,   ERR::InvalidValue,   "Invalid option")
ERRDEF(INVOPTM,  ERR::InvalidValue,   "Invalid option " LUA_QS)
ERRDEF(INVFMT,   ERR::StringFormat,   "Invalid format")
ERRDEF(SETFENV,  ERR::NotPossible,    LUA_QL("setfenv") " cannot change environment of given object")
ERRDEF(TABINS,   ERR::Args,           "Wrong number of arguments to " LUA_QL("insert"))
ERRDEF(TABCAT,   ERR::TypeMismatch,   "Invalid value (%s) at index %d in table for " LUA_QL("concat"))
ERRDEF(TABSORT,  ERR::InvalidValue,   "Invalid order function for sorting")
ERRDEF(TABSEQ,   ERR::TypeMismatch,   LUA_QS "() requires a sequence table; received %s table")
ERRDEF(IOCLFL,   ERR::InvalidState,   "Attempt to use a closed file")
ERRDEF(IOSTDCL,  ERR::InvalidState,   "Standard file is closed")
ERRDEF(OSUNIQF,  ERR::CreateFile,     "Unable to generate a unique filename")
ERRDEF(OSDATEF,  ERR::FieldNotFound,  "Field " LUA_QS " missing in date table")
ERRDEF(STRDUMP,  ERR::NotPossible,    "Unable to dump given function")
ERRDEF(STRSLC,   ERR::BufferOverflow, "String slice too long")
ERRDEF(STRPATB,  ERR::Syntax,         "Missing " LUA_QL("[") " after " LUA_QL("%f") " in pattern")
ERRDEF(STRPATC,  ERR::Syntax,         "Invalid pattern capture")
ERRDEF(STRPATE,  ERR::Syntax,         "Malformed pattern (ends with " LUA_QL("%") ")")
ERRDEF(STRPATM,  ERR::Syntax,         "Malformed pattern (missing " LUA_QL("]") ")")
ERRDEF(STRPATU,  ERR::Syntax,         "Unbalanced pattern")
ERRDEF(STRPATX,  ERR::Syntax,         "Pattern too complex")
ERRDEF(STRCAPI,  ERR::Syntax,         "Invalid capture index")
ERRDEF(STRCAPN,  ERR::Syntax,         "Too many captures")
ERRDEF(STRCAPU,  ERR::Syntax,         "Unfinished capture")
ERRDEF(STRFMT,   ERR::StringFormat,   "Invalid option " LUA_QS " to " LUA_QL("format"))
ERRDEF(STRGSRV,  ERR::TypeMismatch,   "Invalid replacement value (a %s)")
ERRDEF(BADMODN,  ERR::AlreadyDefined, "Name conflict for module " LUA_QS)
ERRDEF(JITPROT,  ERR::NoPermission,   "Runtime code generation failed, restricted kernel?")
ERRDEF(NOJIT,    ERR::NoSupport,      "JIT compiler disabled")
ERRDEF(JITOPT,   ERR::InvalidValue,   "Unknown or malformed optimization flag " LUA_QS)

// Lexer/parser errors.
ERRDEF(XMODE,    ERR::InvalidValue,   "Attempt to load chunk with wrong mode")
ERRDEF(XNEAR,    ERR::Syntax,         "%s near " LUA_QS)
ERRDEF(XLINES,   ERR::BufferOverflow, "Chunk has too many lines")
ERRDEF(XLEVELS,  ERR::BufferOverflow, "Chunk has too many syntax levels")
ERRDEF(XNUMBER,  ERR::Syntax,         "Malformed number")
ERRDEF(XLSTR,    ERR::Syntax,         "Unfinished long string")
ERRDEF(XLCOM,    ERR::Syntax,         "Unfinished long comment")
ERRDEF(XSTR,     ERR::Syntax,         "Unfinished string")
ERRDEF(XESC,     ERR::Syntax,         "Invalid escape sequence")
ERRDEF(XLDELIM,  ERR::Syntax,         "Invalid long string delimiter")
ERRDEF(XTOKEN,   ERR::Syntax,         LUA_QS " expected")
ERRDEF(XJUMP,    ERR::BufferOverflow, "Control structure too long")
ERRDEF(XSLOTS,   ERR::BufferOverflow, "Function or expression too complex, exceeded LJ_MAX_SLOTS")
ERRDEF(XLIMC,    ERR::BufferOverflow, "Chunk has more than %d local variables")
ERRDEF(XLIMM,    ERR::BufferOverflow, "Main function has more than %d %s")
ERRDEF(XLIMF,    ERR::BufferOverflow, "Function at line %d has more than %d %s")
ERRDEF(XMATCH,   ERR::Syntax,         LUA_QS " expected (to close " LUA_QS " at line %d)")
ERRDEF(XFIXUP,   ERR::BufferOverflow, "Function too long for return fixup")
ERRDEF(XPARAM,   ERR::Syntax,         "<name> or " LUA_QL("...") " expected")
ERRDEF(XFUNARG,  ERR::Syntax,         "Function arguments expected")
ERRDEF(XSYMBOL,  ERR::Syntax,         "Unexpected symbol")
ERRDEF(XDOTS,    ERR::Syntax,         "Cannot use " LUA_QL("...") " outside a vararg function")
ERRDEF(XSYNTAX,  ERR::Syntax,         "Syntax error")
ERRDEF(XFOR,     ERR::Syntax,         LUA_QL("=") " or " LUA_QL("in") " expected")
ERRDEF(XBREAK,   ERR::Syntax,         "No loop to break")
ERRDEF(XLEFTCOMPOUND,  ERR::Syntax,   "Syntax error in left hand expression in compound assignment")
ERRDEF(XRIGHTCOMPOUND, ERR::Syntax,   "Syntax error in right hand expression in compound assignment")
ERRDEF(XNOTASSIGNABLE, ERR::Syntax,   "Syntax error expression not assignable")
ERRDEF(XCONTINUE,   ERR::Syntax,      "No loop to continue")
ERRDEF(XBLANKREAD,  ERR::Syntax,      "Cannot read blank identifier " LUA_QL("_"))
ERRDEF(XUNDEF,      ERR::Syntax,      "Undefined variable " LUA_QS)
ERRDEF(XPARSER,     ERR::Syntax,      "%s")
ERRDEF(XLUNDEF,     ERR::Syntax,      "Undefined label " LUA_QS)
ERRDEF(XLDUP,       ERR::Syntax,      "Duplicate label " LUA_QS)
ERRDEF(XFSTR_EMPTY, ERR::Syntax,      "Empty interpolation in f-string")
ERRDEF(XFSTR_BRACE, ERR::Syntax,      "Unclosed brace in f-string interpolation")
ERRDEF(XEMPTYCOMMENT, ERR::Syntax,    "Empty comment appended to variable is not a decrement operation")
ERRDEF(XNEST,       ERR::BufferOverflow, "Try blocks nested too deeply")
ERRDEF(BADLIBRARY,  ERR::Syntax,      "Invalid library name; only alpha-numeric names are permitted with max 96 chars.")

// Bytecode reader errors.
ERRDEF(BCFMT,   ERR::WrongVersion,    "Cannot load incompatible bytecode")
ERRDEF(BCBAD,   ERR::InvalidData,     "Cannot load malformed bytecode")

// Array errors.
ERRDEF(ARROB,   ERR::OutOfBounds,     "Array index %d out of bounds (size %d)")
ERRDEF(ARRRO,   ERR::ReadOnly,        "Attempt to modify read-only array")
ERRDEF(ARRTYPE, ERR::TypeMismatch,    "Invalid array element type")
ERRDEF(ARRSTR,  ERR::TypeMismatch,    "Byte array expected for string extraction")
ERRDEF(ARREXT,  ERR::Immutable,       "Cannot grow external or cached string array")

// Object errors.
ERRDEF(OBJFREED, ERR::DoesNotExist,   "Object has been freed")

ERRDEF(DEPRECATED, ERR::Obsolete,     "Function is deprecated")

#undef ERRDEF

/* Detecting unused error messages:
   awk -F, '/^ERRDEF/ { gsub(/ERRDEF./, ""); printf "grep -q ErrMsg::%s *.[ch] || echo %s\n", $1, $1}' lj_errmsg.h | sh
*/
