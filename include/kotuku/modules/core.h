#pragma once

// Name:      core.h
// Copyright: Paul Manias 1996-2026
// Generator: idl-c

#include <kotuku/main.h>

#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

#ifdef __cplusplus
#include <list>
#include <map>
#include <string>
#include <set>
#include <vector>
#include <unordered_map>
#include <bit>
#include <atomic>
#include <array>
#include <charconv>
#include <sstream>
#include <cmath>
#include <type_traits>
#include "ankerl/unordered_dense.h"
#endif

#ifndef NDEBUG
 #ifndef _MSC_VER
  #include <signal.h>
 #endif
#endif

// For use in requires statements
template <typename T> concept pcPointer = std::is_pointer_v<T>;
template <typename T> concept pcComplete = requires { sizeof(T); };
template <typename T> concept pcObject = pcComplete<T> and std::is_base_of_v<Object, T>;
template <typename T> concept pcObjectPointer = std::is_pointer_v<std::remove_reference_t<T>> and
   pcComplete<std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<T>>>> and
   std::is_base_of_v<Object, std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<T>>>>;

#ifndef DEFINE_ENUM_FLAG_OPERATORS
template <size_t S> struct _ENUM_FLAG_INTEGER_FOR_SIZE;
template <> struct _ENUM_FLAG_INTEGER_FOR_SIZE<1> { typedef int8_t type; };
template <> struct _ENUM_FLAG_INTEGER_FOR_SIZE<2> { typedef int16_t type; };
template <> struct _ENUM_FLAG_INTEGER_FOR_SIZE<4> { typedef int type; };
template <> struct _ENUM_FLAG_INTEGER_FOR_SIZE<8> { typedef int64_t type; };
// used as an approximation of std::underlying_type<T>
template <class T> struct _ENUM_FLAG_SIZED_INTEGER { typedef typename _ENUM_FLAG_INTEGER_FOR_SIZE<sizeof(T)>::type type; };

#define DEFINE_ENUM_FLAG_OPERATORS(ENUMTYPE) \
constexpr ENUMTYPE operator | (ENUMTYPE a, ENUMTYPE b) noexcept { return ENUMTYPE(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)a) | ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); } \
constexpr ENUMTYPE operator & (ENUMTYPE a, ENUMTYPE b) noexcept { return ENUMTYPE(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)a) & ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); } \
constexpr ENUMTYPE operator ~ (ENUMTYPE a) noexcept { return ENUMTYPE(~((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)a)); } \
constexpr ENUMTYPE operator ^ (ENUMTYPE a, ENUMTYPE b) noexcept { return ENUMTYPE(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)a) ^ ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); } \
constexpr ENUMTYPE &operator |= (ENUMTYPE &a, ENUMTYPE b) noexcept { return (ENUMTYPE &)(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type &)a) |= ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); } \
constexpr ENUMTYPE &operator &= (ENUMTYPE &a, ENUMTYPE b) noexcept { return (ENUMTYPE &)(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type &)a) &= ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); }
#endif

template<typename T>
constexpr bool defined(T flags, T test_flag) noexcept {
   static_assert(std::is_enum_v<T>, "Type must be an enum");
   using underlying = std::underlying_type_t<T>;
   return (static_cast<underlying>(flags) & static_cast<underlying>(test_flag)) != 0;
}

constexpr int RESOURCE_ID_OFFSET = -1;
class objMetaClass;
class objStorageDevice;
class objFile;
class objConfig;
class objScript;
class objTask;
class objThread;
class objModule;
class objTime;
class objCompression;
class objCompressedStream;

// Clipboard modes

enum class CLIPMODE : uint32_t {
   NIL = 0,
   CUT = 0x00000001,
   COPY = 0x00000002,
   PASTE = 0x00000004,
};

DEFINE_ENUM_FLAG_OPERATORS(CLIPMODE)

// Seek positions

enum class SEEK : int {
   NIL = 0,
   START = 0,
   CURRENT = 1,
   END = 2,
   RELATIVE = 3,
};

enum class DEVICE : int64_t {
   NIL = 0,
   COMPACT_DISC = 0x00000001,
   HARD_DISK = 0x00000002,
   FLOPPY_DISK = 0x00000004,
   READ = 0x00000008,
   WRITE = 0x00000010,
   REMOVABLE = 0x00000020,
   REMOVEABLE = 0x00000020,
   SOFTWARE = 0x00000040,
   NETWORK = 0x00000080,
   TAPE = 0x00000100,
   PRINTER = 0x00000200,
   SCANNER = 0x00000400,
   TEMPORARY = 0x00000800,
   MEMORY = 0x00001000,
   MODEM = 0x00002000,
   USB = 0x00004000,
   FIXED = 0x00008000,
   PRINTER_3D = 0x00010000,
   SCANNER_3D = 0x00020000,
   BOOKMARK = 0x00040000,
};

DEFINE_ENUM_FLAG_OPERATORS(DEVICE)

// Class categories

enum class CCF : uint32_t {
   NIL = 0,
   COMMAND = 0x00000001,
   FILESYSTEM = 0x00000002,
   GRAPHICS = 0x00000004,
   GUI = 0x00000008,
   IO = 0x00000010,
   SYSTEM = 0x00000020,
   TOOL = 0x00000040,
   AUDIO = 0x00000080,
   DATA = 0x00000100,
   MISC = 0x00000200,
   NETWORK = 0x00000400,
   MULTIMEDIA = 0x00000800,
};

DEFINE_ENUM_FLAG_OPERATORS(CCF)

// Action identifiers.

enum class AC : int {
   NIL = 0,
   Signal = 1,
   Activate = 2,
   Redimension = 3,
   Clear = 4,
   FreeWarning = 5,
   Enable = 6,
   CopyData = 7,
   DataFeed = 8,
   Deactivate = 9,
   Draw = 10,
   Flush = 11,
   Focus = 12,
   Free = 13,
   SaveSettings = 14,
   GetKey = 15,
   DragDrop = 16,
   Hide = 17,
   Init = 18,
   Lock = 19,
   LostFocus = 20,
   Move = 21,
   MoveToBack = 22,
   MoveToFront = 23,
   NewChild = 24,
   NewOwner = 25,
   NewObject = 26,
   Redo = 27,
   Query = 28,
   Read = 29,
   Rename = 30,
   Reset = 31,
   Resize = 32,
   SaveImage = 33,
   SaveToObject = 34,
   MoveToPoint = 35,
   Seek = 36,
   SetKey = 37,
   Show = 38,
   Undo = 39,
   Unlock = 40,
   Next = 41,
   Prev = 42,
   Write = 43,
   SetField = 44,
   Clipboard = 45,
   Refresh = 46,
   Disable = 47,
   NewPlacement = 48,
   FreePlacement = 49,
   END = 50,
};

// Permission flags

enum class PERMIT : uint32_t {
   NIL = 0,
   READ = 0x00000001,
   USER_READ = 0x00000001,
   WRITE = 0x00000002,
   USER_WRITE = 0x00000002,
   EXEC = 0x00000004,
   USER_EXEC = 0x00000004,
   DELETE = 0x00000008,
   USER = 0x0000000f,
   GROUP_READ = 0x00000010,
   GROUP_WRITE = 0x00000020,
   GROUP_EXEC = 0x00000040,
   GROUP_DELETE = 0x00000080,
   GROUP = 0x000000f0,
   OTHERS_READ = 0x00000100,
   EVERYONE_READ = 0x00000111,
   ALL_READ = 0x00000111,
   OTHERS_WRITE = 0x00000200,
   EVERYONE_WRITE = 0x00000222,
   ALL_WRITE = 0x00000222,
   EVERYONE_READWRITE = 0x00000333,
   OTHERS_EXEC = 0x00000400,
   ALL_EXEC = 0x00000444,
   EVERYONE_EXEC = 0x00000444,
   OTHERS_DELETE = 0x00000800,
   ALL_DELETE = 0x00000888,
   EVERYONE_DELETE = 0x00000888,
   OTHERS = 0x00000f00,
   EVERYONE_ACCESS = 0x00000fff,
   HIDDEN = 0x00001000,
   ARCHIVE = 0x00002000,
   PASSWORD = 0x00004000,
   USERID = 0x00008000,
   GROUPID = 0x00010000,
   INHERIT = 0x00020000,
   OFFLINE = 0x00040000,
   NETWORK = 0x00080000,
   META = 0x000c5000,
};

DEFINE_ENUM_FLAG_OPERATORS(PERMIT)

// Special qualifier flags

enum class KQ : uint32_t {
   NIL = 0,
   L_SHIFT = 0x00000001,
   R_SHIFT = 0x00000002,
   SHIFT = 0x00000003,
   CAPS_LOCK = 0x00000004,
   L_CONTROL = 0x00000008,
   L_CTRL = 0x00000008,
   R_CTRL = 0x00000010,
   R_CONTROL = 0x00000010,
   CTRL = 0x00000018,
   CONTROL = 0x00000018,
   L_ALT = 0x00000020,
   ALTGR = 0x00000040,
   R_ALT = 0x00000040,
   ALT = 0x00000060,
   INSTRUCTION_KEYS = 0x00000078,
   L_COMMAND = 0x00000080,
   R_COMMAND = 0x00000100,
   COMMAND = 0x00000180,
   QUALIFIERS = 0x000001fb,
   NUM_PAD = 0x00000200,
   REPEAT = 0x00000400,
   RELEASED = 0x00000800,
   PRESSED = 0x00001000,
   NOT_PRINTABLE = 0x00002000,
   INFO = 0x00003c04,
   SCR_LOCK = 0x00004000,
   NUM_LOCK = 0x00008000,
   DEAD_KEY = 0x00010000,
   WIN_CONTROL = 0x00020000,
};

DEFINE_ENUM_FLAG_OPERATORS(KQ)

// Memory types used by AllocMemory().  The lower 16 bits are stored with allocated blocks, the upper 16 bits are function-relative only.

enum class MEM : uint32_t {
   NIL = 0,
   DATA = 0x00000000,
   VIDEO = 0x00000001,
   TEXTURE = 0x00000002,
   AUDIO = 0x00000004,
   CODE = 0x00000008,
   UNTRACKED = 0x00000010,
   STRING = 0x00000020,
   COLLECT = 0x00000040,
   PROTECTED = 0x00000080,
   READ = 0x00010000,
   WRITE = 0x00020000,
   READ_WRITE = 0x00030000,
   NO_CLEAR = 0x00040000,
};

DEFINE_ENUM_FLAG_OPERATORS(MEM)

// Event categories.

enum class EVG : int {
   NIL = 0,
   FILESYSTEM = 1,
   NETWORK = 2,
   SYSTEM = 3,
   GUI = 4,
   DISPLAY = 5,
   IO = 6,
   HARDWARE = 7,
   AUDIO = 8,
   USER = 9,
   POWER = 10,
   CLASS = 11,
   APP = 12,
   ANDROID = 13,
   END = 14,
};

// Data codes

enum class DATA : int {
   NIL = 0,
   TEXT = 1,
   RAW = 2,
   DEVICE_INPUT = 3,
   XML = 4,
   AUDIO = 5,
   RECORD = 6,
   IMAGE = 7,
   REQUEST = 8,
   RECEIPT = 9,
   FILE = 10,
   CONTENT = 11,
   INPUT_READY = 12,
};

// JTYPE flags are used to categorise input types.

enum class JTYPE : uint32_t {
   NIL = 0,
   SECONDARY = 0x00000001,
   ANCHORED = 0x00000002,
   DRAGGED = 0x00000004,
   CROSSING = 0x00000008,
   DIGITAL = 0x00000010,
   ANALOG = 0x00000020,
   EXT_MOVEMENT = 0x00000040,
   BUTTON = 0x00000080,
   MOVEMENT = 0x00000100,
   DBL_CLICK = 0x00000200,
   REPEATED = 0x00000400,
   DRAG_ITEM = 0x00000800,
};

DEFINE_ENUM_FLAG_OPERATORS(JTYPE)

// JET constants are documented in GetInputEvent()

enum class JET : int {
   NIL = 0,
   BUTTON_1 = 1,
   LMB = 1,
   BUTTON_2 = 2,
   RMB = 2,
   BUTTON_3 = 3,
   MMB = 3,
   BUTTON_4 = 4,
   BUTTON_5 = 5,
   BUTTON_6 = 6,
   BUTTON_7 = 7,
   BUTTON_8 = 8,
   BUTTON_9 = 9,
   BUTTON_10 = 10,
   WHEEL = 11,
   WHEEL_TILT = 12,
   PEN_TILT_XY = 13,
   ABS_XY = 14,
   CROSSED_IN = 15,
   CROSSED_OUT = 16,
   PRESSURE = 17,
   DEVICE_TILT_XY = 18,
   DEVICE_TILT_Z = 19,
   DISPLAY_EDGE = 20,
   END = 21,
};

// Field descriptors.

#define FD_DOUBLERESULT 0x80000100
#define FD_PTR_DOUBLERESULT 0x88000100
#define FD_CLASS_TYPES 0xffc05001
#define FD_VOID 0x00000000
#define FD_OBJECT 0x00000001
#define FD_LOCAL 0x00000002
#define FD_VIRTUAL 0x00000008
#define FD_MUTABLE 0x00000008
#define FD_STRUCT 0x00000010
#define FD_ALLOC 0x00000020
#define FD_FLAGS 0x00000040
#define FD_VARTAGS 0x00000040
#define FD_PTRSIZE 0x00000080
#define FD_ARRAYSIZE 0x00000080
#define FD_LOOKUP 0x00000080
#define FD_BUFSIZE 0x00000080
#define FD_R 0x00000100
#define FD_READ 0x00000100
#define FD_RESULT 0x00000100
#define FD_WRITE 0x00000200
#define FD_W 0x00000200
#define FD_BUFFER 0x00000200
#define FD_RW 0x00000300
#define FD_I 0x00000400
#define FD_TAGS 0x00000400
#define FD_INIT 0x00000400
#define FD_RI 0x00000500
#define FD_ERROR 0x00000800
#define FD_ARRAY 0x00001000
#define FD_RESOURCE 0x00002000
#define FD_CPP 0x00004000
#define FD_CUSTOM 0x00008000
#define FD_SYSTEM 0x00010000
#define FD_PRIVATE 0x00010000
#define FD_SYNONYM 0x00020000
#define FD_UNSIGNED 0x00040000
#define FD_PURE 0x00100000
#define FD_SCALED 0x00200000
#define FD_NORMALISED 0x00200000
#define FD_WORD 0x00400000
#define FD_STRING 0x00800000
#define FD_STR 0x00800000
#define FD_STRRESULT 0x00800100
#define FD_BYTE 0x01000000
#define FD_FUNCTION 0x02000000
#define FD_INT64 0x04000000
#define FD_INT64RESULT 0x04000100
#define FD_POINTER 0x08000000
#define FD_PTR 0x08000000
#define FD_OBJECTPTR 0x08000001
#define FD_PTRRESULT 0x08000100
#define FD_PTRBUFFER 0x08000200
#define FD_FUNCTIONPTR 0x0a000000
#define FD_PTR_INT64RESULT 0x0c000100
#define FD_FLOAT 0x10000000
#define FD_UNIT 0x20000000
#define FD_INT 0x40000000
#define FD_OBJECTID 0x40000001
#define FD_INTRESULT 0x40000100
#define FD_PTR_INTRESULT 0x48000100
#define FD_DOUBLE 0x80000000

// Predefined cursor styles

enum class PTC : int {
   NIL = 0,
   NO_CHANGE = 0,
   DEFAULT = 1,
   SIZE_BOTTOM_LEFT = 2,
   SIZE_BOTTOM_RIGHT = 3,
   SIZE_TOP_LEFT = 4,
   SIZE_TOP_RIGHT = 5,
   SIZE_LEFT = 6,
   SIZE_RIGHT = 7,
   SIZE_TOP = 8,
   SIZE_BOTTOM = 9,
   CROSSHAIR = 10,
   SLEEP = 11,
   SIZING = 12,
   SPLIT_VERTICAL = 13,
   SPLIT_HORIZONTAL = 14,
   MAGNIFIER = 15,
   HAND = 16,
   HAND_LEFT = 17,
   HAND_RIGHT = 18,
   TEXT = 19,
   PAINTBRUSH = 20,
   STOP = 21,
   INVISIBLE = 22,
   CUSTOM = 23,
   DRAGGABLE = 24,
   END = 25,
};

enum class DMF : uint32_t {
   NIL = 0,
   SCALED_X = 0x00000001,
   SCALED_Y = 0x00000002,
   FIXED_X = 0x00000004,
   FIXED_Y = 0x00000008,
   SCALED_X_OFFSET = 0x00000010,
   SCALED_Y_OFFSET = 0x00000020,
   FIXED_X_OFFSET = 0x00000040,
   FIXED_Y_OFFSET = 0x00000080,
   FIXED_HEIGHT = 0x00000100,
   FIXED_WIDTH = 0x00000200,
   SCALED_HEIGHT = 0x00000400,
   SCALED_WIDTH = 0x00000800,
   FIXED_DEPTH = 0x00001000,
   SCALED_DEPTH = 0x00002000,
   FIXED_Z = 0x00004000,
   SCALED_Z = 0x00008000,
   SCALED_RADIUS_X = 0x00010000,
   FIXED_RADIUS_X = 0x00020000,
   SCALED_CENTER_X = 0x00040000,
   SCALED_CENTER_Y = 0x00080000,
   FIXED_CENTER_X = 0x00100000,
   FIXED_CENTER_Y = 0x00200000,
   STATUS_CHANGE_H = 0x00400000,
   STATUS_CHANGE_V = 0x00800000,
   SCALED_RADIUS_Y = 0x01000000,
   FIXED_RADIUS_Y = 0x02000000,
};

DEFINE_ENUM_FLAG_OPERATORS(DMF)

// Compass directions.

enum class DRL : int {
   NIL = 0,
   NORTH = 0,
   UP = 0,
   SOUTH = 1,
   DOWN = 1,
   EAST = 2,
   RIGHT = 2,
   WEST = 3,
   LEFT = 3,
   NORTH_EAST = 4,
   NORTH_WEST = 5,
   SOUTH_EAST = 6,
   SOUTH_WEST = 7,
};

// Generic flags for controlling movement.

enum class MOVE : uint32_t {
   NIL = 0,
   DOWN = 0x00000001,
   UP = 0x00000002,
   LEFT = 0x00000004,
   RIGHT = 0x00000008,
   ALL = 0x0000000f,
};

DEFINE_ENUM_FLAG_OPERATORS(MOVE)

// Edge flags

enum class EDGE : uint32_t {
   NIL = 0,
   TOP = 0x00000001,
   LEFT = 0x00000002,
   RIGHT = 0x00000004,
   BOTTOM = 0x00000008,
   TOP_LEFT = 0x00000010,
   TOP_RIGHT = 0x00000020,
   BOTTOM_LEFT = 0x00000040,
   BOTTOM_RIGHT = 0x00000080,
   ALL = 0x000000ff,
};

DEFINE_ENUM_FLAG_OPERATORS(EDGE)

// Universal values for alignment of graphics and text

enum class ALIGN : uint32_t {
   NIL = 0,
   LEFT = 0x00000001,
   RIGHT = 0x00000002,
   HORIZONTAL = 0x00000004,
   VERTICAL = 0x00000008,
   MIDDLE = 0x0000000c,
   CENTER = 0x0000000c,
   TOP = 0x00000010,
   BOTTOM = 0x00000020,
};

DEFINE_ENUM_FLAG_OPERATORS(ALIGN)

// Universal values for alignment of graphic layouts in documents.

enum class LAYOUT : uint32_t {
   NIL = 0,
   SQUARE = 0x00000000,
   TIGHT = 0x00000001,
   LEFT = 0x00000002,
   RIGHT = 0x00000004,
   WIDE = 0x00000006,
   BACKGROUND = 0x00000008,
   FOREGROUND = 0x00000010,
   EMBEDDED = 0x00000020,
   LOCK = 0x00000040,
   IGNORE_CURSOR = 0x00000080,
   TILE = 0x00000100,
};

DEFINE_ENUM_FLAG_OPERATORS(LAYOUT)

// Script flags

enum class SCF : uint32_t {
   NIL = 0,
   EXIT_ON_ERROR = 0x00000001,
   PROCESS_DOC = 0x00000002,
   LOG_ALL = 0x00000004,
};

DEFINE_ENUM_FLAG_OPERATORS(SCF)

enum class STR : uint32_t {
   NIL = 0,
   MATCH_CASE = 0x00000001,
   CASE = 0x00000001,
   MATCH_LEN = 0x00000002,
};

DEFINE_ENUM_FLAG_OPERATORS(STR)

// Message flags.

enum class MSF : uint32_t {
   NIL = 0,
   UPDATE = 0x00000001,
   NO_DUPLICATE = 0x00000002,
};

DEFINE_ENUM_FLAG_OPERATORS(MSF)

// Flags for ProcessMessages

enum class PMF : uint32_t {
   NIL = 0,
   EVENT_LOOP = 0x00000001,
   SYSTEM_NO_BREAK = 0x00000002,
};

DEFINE_ENUM_FLAG_OPERATORS(PMF)

// Flags for RegisterFD()

enum class RFD : uint32_t {
   NIL = 0,
   WRITE = 0x00000001,
   EXCEPT = 0x00000002,
   READ = 0x00000004,
   REMOVE = 0x00000008,
   STOP_RECURSE = 0x00000010,
   ALLOW_RECURSION = 0x00000020,
   SOCKET = 0x00000040,
   RECALL = 0x00000080,
   ALWAYS_CALL = 0x00000100,
};

DEFINE_ENUM_FLAG_OPERATORS(RFD)

// Task flags

enum class TSF : uint32_t {
   NIL = 0,
   WAIT = 0x00000001,
   RESET_PATH = 0x00000002,
   PRIVILEGED = 0x00000004,
   SHELL = 0x00000008,
   VERBOSE = 0x00000010,
   QUIET = 0x00000020,
   DETACHED = 0x00000040,
   ATTACHED = 0x00000080,
   PIPE = 0x00000100,
};

DEFINE_ENUM_FLAG_OPERATORS(TSF)

// Internal options for requesting function tables from modules.

enum class MHF : uint32_t {
   NIL = 0,
   STATIC = 0x00000001,
   STRUCTURE = 0x00000002,
   DEFAULT = 0x00000002,
};

DEFINE_ENUM_FLAG_OPERATORS(MHF)

// MoveToPoint flags

enum class MTF : uint32_t {
   NIL = 0,
   X = 0x00000001,
   Y = 0x00000002,
   Z = 0x00000004,
   ANIM = 0x00000008,
   RELATIVE = 0x00000010,
};

DEFINE_ENUM_FLAG_OPERATORS(MTF)

// VlogF flags

enum class VLF : uint32_t {
   NIL = 0,
   BRANCH = 0x00000001,
   ERROR = 0x00000002,
   WARNING = 0x00000004,
   CRITICAL = 0x00000008,
   INFO = 0x00000010,
   API = 0x00000020,
   DETAIL = 0x00000040,
   TRACE = 0x00000080,
   FUNCTION = 0x00000100,
};

DEFINE_ENUM_FLAG_OPERATORS(VLF)

// Module flags

enum class MOF : uint32_t {
   NIL = 0,
   LINK_LIBRARY = 0x00000001,
   STATIC = 0x00000002,
   SYSTEM_PROBE = 0x00000004,
};

DEFINE_ENUM_FLAG_OPERATORS(MOF)

// Thread flags

enum class THF : uint32_t {
   NIL = 0,
   AUTO_FREE = 0x00000001,
};

DEFINE_ENUM_FLAG_OPERATORS(THF)

// Flags for the SetDate() file method.

enum class FDT : int {
   NIL = 0,
   MODIFIED = 0,
   CREATED = 1,
   ACCESSED = 2,
   ARCHIVED = 3,
};

// Options for SetVolume()

enum class VOLUME : uint32_t {
   NIL = 0,
   REPLACE = 0x00000001,
   PRIORITY = 0x00000002,
   HIDDEN = 0x00000004,
   SYSTEM = 0x00000008,
};

DEFINE_ENUM_FLAG_OPERATORS(VOLUME)

// Options for the File Delete() method.

enum class FDL : uint32_t {
   NIL = 0,
   FEEDBACK = 0x00000001,
};

DEFINE_ENUM_FLAG_OPERATORS(FDL)

// Compression flags

enum class CMF : uint32_t {
   NIL = 0,
   PASSWORD = 0x00000001,
   NEW = 0x00000002,
   CREATE_FILE = 0x00000004,
   READ_ONLY = 0x00000008,
   NO_LINKS = 0x00000010,
   APPLY_SECURITY = 0x00000020,
};

DEFINE_ENUM_FLAG_OPERATORS(CMF)

// Flags for ResolvePath()

enum class RSF : uint32_t {
   NIL = 0,
   NO_FILE_CHECK = 0x00000001,
   CHECK_VIRTUAL = 0x00000002,
   APPROXIMATE = 0x00000004,
   NO_DEEP_SCAN = 0x00000008,
   PATH = 0x00000010,
   CASE_SENSITIVE = 0x00000020,
};

DEFINE_ENUM_FLAG_OPERATORS(RSF)

// Flags for the File Watch() method.

enum class MFF : uint32_t {
   NIL = 0,
   READ = 0x00000001,
   MODIFY = 0x00000002,
   WRITE = 0x00000002,
   CREATE = 0x00000004,
   DELETE = 0x00000008,
   MOVED = 0x00000010,
   RENAME = 0x00000010,
   ATTRIB = 0x00000020,
   OPENED = 0x00000040,
   CLOSED = 0x00000080,
   UNMOUNT = 0x00000100,
   FOLDER = 0x00000200,
   FILE = 0x00000400,
   SELF = 0x00000800,
   DEEP = 0x00001000,
};

DEFINE_ENUM_FLAG_OPERATORS(MFF)

// Types for StrDatatype().

enum class STT : int {
   NIL = 0,
   NUMBER = 1,
   FLOAT = 2,
   HEX = 3,
   STRING = 4,
};

enum class OPF : uint32_t {
   NIL = 0,
   OPTIONS = 0x00000001,
   MAX_DEPTH = 0x00000002,
   DETAIL = 0x00000004,
   SHOW_MEMORY = 0x00000008,
   SHOW_IO = 0x00000010,
   SHOW_ERRORS = 0x00000020,
   ARGS = 0x00000040,
   PRIVILEGED = 0x00000080,
   SYSTEM_PATH = 0x00000100,
   MODULE_PATH = 0x00000200,
   ROOT_PATH = 0x00000400,
   SCAN_MODULES = 0x00000800,
};

DEFINE_ENUM_FLAG_OPERATORS(OPF)

// Console types

enum class CONTYPE : int {
   NIL = 0,
   NONE = 0,
   TERMINAL = 1,
   HANDLE = 2,
   MANUAL = 3,
};

enum class TOI : int {
   NIL = 0,
   LOCAL_CACHE = 0,
   LOCAL_STORAGE = 1,
   ANDROID_ENV = 2,
   ANDROID_CLASS = 3,
   ANDROID_ASSETMGR = 4,
};

// Flags for the OpenDir() function.

enum class RDF : uint32_t {
   NIL = 0,
   SIZE = 0x00000001,
   DATE = 0x00000002,
   TIME = 0x00000002,
   PERMISSIONS = 0x00000004,
   FILES = 0x00000008,
   FILE = 0x00000008,
   FOLDERS = 0x00000010,
   FOLDER = 0x00000010,
   READ_ALL = 0x0000001f,
   VOLUME = 0x00000020,
   LINK = 0x00000040,
   TAGS = 0x00000080,
   QUALIFIED = 0x00000100,
   QUALIFY = 0x00000100,
   VIRTUAL = 0x00000200,
   STREAM = 0x00000400,
   READ_ONLY = 0x00000800,
   OPENDIR = 0x00001000,
};

DEFINE_ENUM_FLAG_OPERATORS(RDF)

// File flags

enum class FL : uint32_t {
   NIL = 0,
   WRITE = 0x00000001,
   NEW = 0x00000002,
   READ = 0x00000004,
   DIRECTORY = 0x00000008,
   FOLDER = 0x00000008,
   APPROXIMATE = 0x00000010,
   LINK = 0x00000020,
   BUFFER = 0x00000040,
   LOOP = 0x00000080,
   FILE = 0x00000100,
   RESET_DATE = 0x00000200,
   DEVICE = 0x00000400,
   STREAM = 0x00000800,
   EXCLUDE_FILES = 0x00001000,
   EXCLUDE_FOLDERS = 0x00002000,
   VIRTUAL = 0x00004000,
};

DEFINE_ENUM_FLAG_OPERATORS(FL)

// AnalysePath() values

enum class LOC : int {
   NIL = 0,
   DIRECTORY = 1,
   FOLDER = 1,
   VOLUME = 2,
   FILE = 3,
};

// Flags for LoadFile()

enum class LDF : uint32_t {
   NIL = 0,
   CHECK_EXISTS = 0x00000001,
};

DEFINE_ENUM_FLAG_OPERATORS(LDF)

// Flags for file feedback.

enum class FBK : int {
   NIL = 0,
   MOVE_FILE = 1,
   COPY_FILE = 2,
   DELETE_FILE = 3,
};

// Return codes available to the feedback routine

enum class FFR : int {
   NIL = 0,
   OKAY = 0,
   CONTINUE = 0,
   SKIP = 1,
   ABORT = 2,
};

// For use by VirtualVolume()

enum class VAS : int {
   NIL = 0,
   DEREGISTER = 1,
   SCAN_DIR = 2,
   DELETE = 3,
   RENAME = 4,
   OPEN_DIR = 5,
   CLOSE_DIR = 6,
   TEST_PATH = 7,
   WATCH_PATH = 8,
   IGNORE_FILE = 9,
   GET_INFO = 10,
   GET_DEVICE_INFO = 11,
   IDENTIFY_FILE = 12,
   MAKE_DIR = 13,
   SAME_FILE = 14,
   CASE_SENSITIVE = 15,
   READ_LINK = 16,
   CREATE_LINK = 17,
   DRIVER_SIZE = 18,
};

// Feedback event indicators.

enum class FDB : int {
   NIL = 0,
   DECOMPRESS_FILE = 1,
   COMPRESS_FILE = 2,
   REMOVE_FILE = 3,
   DECOMPRESS_OBJECT = 4,
};

// Compression stream formats

enum class CF : int {
   NIL = 0,
   GZIP = 1,
   ZLIB = 2,
   DEFLATE = 3,
};

// Flags that can be passed to NewObject().  If a flag needs to be stored with the object, it must be specified in the lower word.

enum class NF : uint32_t {
   NIL = 0,
   PRIVATE = 0x00000000,
   UNTRACKED = 0x00000001,
   INITIALISED = 0x00000002,
   LOCAL = 0x00000004,
   FREE_ON_UNLOCK = 0x00000008,
   FREE = 0x00000010,
   TIMER_SUB = 0x00000020,
   SUPPRESS_LOG = 0x00000040,
   RECLASSED = 0x00000080,
   SIGNALLED = 0x00000100,
   PERMIT_TERMINATE = 0x00000200,
   ASYNC_ACTIVE = 0x00000400,
   UNIQUE = 0x40000000,
   NAME = 0x80000000,
};

DEFINE_ENUM_FLAG_OPERATORS(NF)

#define MAX_NAME_LEN 31
#define MAX_FILENAME 256

// Reserved message ID's that are handled internally.

enum class MSGID : int {
   NIL = 0,
   WAIT_FOR_OBJECTS = 91,
   TIRI_THREAD_CALLBACK = 92,
   THREAD_ACTION = 93,
   THREAD_CALLBACK = 94,
   VALIDATE_PROCESS = 95,
   EVENT = 96,
   DEBUG = 97,
   FREE = 98,
   ACTION = 99,
   BREAK = 100,
   CORE_END = 100,
   COMMAND = 101,
   QUIT = 1000,
};

// Types for AllocateID()

enum class IDTYPE : int {
   NIL = 0,
   MESSAGE = 1,
   GLOBAL = 2,
   FUNCTION = 3,
   RESOURCE = 4,
};

// Indicates the state of a process.

enum class TSTATE : int8_t {
   NIL = 0,
   RUNNING = 0,
   PAUSED = 1,
   STOPPING = 2,
   TERMINATED = 3,
};

enum class RES : int {
   NIL = 0,
   FREE_SWAP = 1,
   CONSOLE_FD = 2,
   KEY_STATE = 3,
   USER_ID = 4,
   DISPLAY_DRIVER = 5,
   PRIVILEGED_USER = 6,
   PRIVILEGED = 7,
   LOG_LEVEL = 8,
   TOTAL_SHARED_MEMORY = 9,
   LOG_DEPTH = 10,
   JNI_ENV = 11,
   THREAD_ID = 12,
   PROCESS_STATE = 13,
   TOTAL_MEMORY = 14,
   TOTAL_SWAP = 15,
   CPU_SPEED = 16,
   FREE_MEMORY = 17,
   MEMORY_USAGE = 18,
   MAIN_THREAD = 19,
   MAIN_THREAD_ID = 20,
};

// Path types for SetResourcePath()

enum class RP : int {
   NIL = 0,
   MODULE_PATH = 1,
   SYSTEM_PATH = 2,
   ROOT_PATH = 3,
};

// Flags for the MetaClass.

enum class CLF : uint32_t {
   NIL = 0,
   INHERIT_LOCAL = 0x00000001,
   NO_OWNERSHIP = 0x00000002,
};

DEFINE_ENUM_FLAG_OPERATORS(CLF)

// Flags for the Config class.

enum class CNF : uint32_t {
   NIL = 0,
   STRIP_QUOTES = 0x00000001,
   AUTO_SAVE = 0x00000002,
   OPTIONAL_FILES = 0x00000004,
   NEW = 0x00000008,
};

DEFINE_ENUM_FLAG_OPERATORS(CNF)

// Raw key codes

enum class KEY : int {
   NIL = 0,
   A = 1,
   B = 2,
   C = 3,
   D = 4,
   E = 5,
   F = 6,
   G = 7,
   H = 8,
   I = 9,
   J = 10,
   K = 11,
   L = 12,
   M = 13,
   N = 14,
   O = 15,
   P = 16,
   Q = 17,
   R = 18,
   S = 19,
   T = 20,
   U = 21,
   V = 22,
   W = 23,
   X = 24,
   Y = 25,
   Z = 26,
   ONE = 27,
   TWO = 28,
   THREE = 29,
   FOUR = 30,
   FIVE = 31,
   SIX = 32,
   SEVEN = 33,
   EIGHT = 34,
   NINE = 35,
   ZERO = 36,
   REVERSE_QUOTE = 37,
   MINUS = 38,
   EQUALS = 39,
   L_SQUARE = 40,
   R_SQUARE = 41,
   SEMI_COLON = 42,
   APOSTROPHE = 43,
   COMMA = 44,
   DOT = 45,
   PERIOD = 45,
   SLASH = 46,
   BACK_SLASH = 47,
   SPACE = 48,
   NP_0 = 49,
   NP_1 = 50,
   NP_2 = 51,
   NP_3 = 52,
   NP_4 = 53,
   NP_5 = 54,
   NP_6 = 55,
   NP_7 = 56,
   NP_8 = 57,
   NP_9 = 58,
   NP_MULTIPLY = 59,
   NP_PLUS = 60,
   NP_BAR = 61,
   NP_SEPARATOR = 61,
   NP_MINUS = 62,
   NP_DECIMAL = 63,
   NP_DOT = 63,
   NP_DIVIDE = 64,
   L_CONTROL = 65,
   R_CONTROL = 66,
   HELP = 67,
   L_SHIFT = 68,
   R_SHIFT = 69,
   CAPS_LOCK = 70,
   PRINT = 71,
   L_ALT = 72,
   R_ALT = 73,
   L_COMMAND = 74,
   R_COMMAND = 75,
   F1 = 76,
   F2 = 77,
   F3 = 78,
   F4 = 79,
   F5 = 80,
   F6 = 81,
   F7 = 82,
   F8 = 83,
   F9 = 84,
   F10 = 85,
   F11 = 86,
   F12 = 87,
   F13 = 88,
   F14 = 89,
   F15 = 90,
   F16 = 91,
   F17 = 92,
   MACRO = 93,
   NP_PLUS_MINUS = 94,
   LESS_GREATER = 95,
   UP = 96,
   DOWN = 97,
   RIGHT = 98,
   LEFT = 99,
   SCR_LOCK = 100,
   PAUSE = 101,
   WAKE = 102,
   SLEEP = 103,
   POWER = 104,
   BACKSPACE = 105,
   TAB = 106,
   ENTER = 107,
   ESCAPE = 108,
   DELETE = 109,
   CLEAR = 110,
   HOME = 111,
   PAGE_UP = 112,
   PAGE_DOWN = 113,
   END = 114,
   SELECT = 115,
   EXECUTE = 116,
   INSERT = 117,
   UNDO = 118,
   REDO = 119,
   MENU = 120,
   FIND = 121,
   CANCEL = 122,
   BREAK = 123,
   NUM_LOCK = 124,
   PRT_SCR = 125,
   NP_ENTER = 126,
   SYSRQ = 127,
   F18 = 128,
   F19 = 129,
   F20 = 130,
   WIN_CONTROL = 131,
   VOLUME_UP = 132,
   VOLUME_DOWN = 133,
   BACK = 134,
   CALL = 135,
   END_CALL = 136,
   CAMERA = 137,
   AT = 138,
   PLUS = 139,
   LENS_FOCUS = 140,
   STOP = 141,
   NEXT = 142,
   PREVIOUS = 143,
   FORWARD = 144,
   REWIND = 145,
   MUTE = 146,
   STAR = 147,
   POUND = 148,
   PLAY = 149,
   LIST_END = 150,
};

struct InputEvent {
   const struct InputEvent * Next;    // Next event in the chain
   double   Value;                    // The value associated with the Type
   int64_t  Timestamp;                // PreciseTime() of the recorded input
   OBJECTID RecipientID;              // Surface that the input message is being conveyed to
   OBJECTID OverID;                   // Surface that is directly under the mouse pointer at the time of the event
   double   AbsX;                     // Absolute horizontal position of mouse cursor (relative to the top left of the display)
   double   AbsY;                     // Absolute vertical position of mouse cursor (relative to the top left of the display)
   double   X;                        // Horizontal position relative to the surface that the pointer is over - unless a mouse button is held or pointer is anchored - then the coordinates are relative to the click-held surface
   double   Y;                        // Vertical position relative to the surface that the pointer is over - unless a mouse button is held or pointer is anchored - then the coordinates are relative to the click-held surface
   OBJECTID DeviceID;                 // The hardware device that this event originated from
   JET      Type;                     // JET constant that describes the event
   JTYPE    Flags;                    // Broad descriptors for the given Type (see JTYPE flags).  Automatically defined when delivered to the pointer object
   JTYPE    Mask;                     // Mask to use for checking against subscribers
};

struct dcRequest {
   int  Item;             // Identifier for retrieval from the source
   char Preference[4];    // Data preferences for the returned item(s)
};

struct dcAudio {
   int Size;    // Byte size of this structure
   int Format;  // Format of the audio data
};

struct dcKeyEntry {
   int     Flags;        // Shift/Control/CapsLock...
   int     Value;        // ASCII value of the key A/B/C/D...
   int64_t Timestamp;    // ~Core.PreciseTime() at which the keypress was recorded
   int     Unicode;      // Unicode value for pre-calculated key translations
};

struct dcDeviceInput {
   double   Values[2];  // The value(s) associated with the Type
   int64_t  Timestamp;  // ~Core.PreciseTime() of the recorded input
   OBJECTID DeviceID;   // The hardware device that this event originated from (note: This ID can be to a private/inaccessible object, the point is that the ID is unique)
   JTYPE    Flags;      // Broad descriptors for the given Type.  Automatically defined when delivered to the pointer object
   JET      Type;       // JET constant
};

struct DateTime {
   int16_t Year;       // Year
   int8_t  Month;      // Month 1 to 12
   int8_t  Day;        // Day 1 to 31
   int8_t  Hour;       // Hour 0 to 23
   int8_t  Minute;     // Minute 0 to 59
   int8_t  Second;     // Second 0 to 59
   int8_t  TimeZone;   // TimeZone -13 to +13
   inline void clear() {
      Year      = 0;
      Month     = 0;
      Day       = 0;
      Hour      = 0;
      Minute    = 0;
      Second    = 0;
      TimeZone  = 0;
   }
};

struct HSV {
   double Hue;           // Between 0 and 359.999
   double Saturation;    // Between 0 and 1.0
   double Value;         // Between 0 and 1.0.  Corresponds to Value, Lightness or Brightness
   double Alpha;         // Alpha blending value from 0 to 1.0
};

// Forward declarations
struct RGB8;

struct FRGB {
   float Red;    // Red component value
   float Green;  // Green component value
   float Blue;   // Blue component value
   float Alpha;  // Alpha component value
   // Note that the colour components are unclamped so that any given FRGB value can support other colour spaces.
   // If targeting a colour space like Display P3 for instance, the expectation is that colour values will exceed
   // 1.0 and negative values are permitted.

   constexpr FRGB() noexcept = default;
   constexpr FRGB(float R, float G, float B, float A = 1.0) noexcept : Red(R), Green(G), Blue(B), Alpha(A) { };

   constexpr RGB8 toRGB8() const noexcept;
};

typedef struct RGB8 {
   uint8_t Red;    // Red component value
   uint8_t Green;  // Green component value
   uint8_t Blue;   // Blue component value
   uint8_t Alpha;  // Alpha component value
   constexpr RGB8() noexcept = default;
   constexpr RGB8(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) noexcept
      : Red(r), Green(g), Blue(b), Alpha(a) { }
   constexpr RGB8(FRGB frgb) noexcept;

   inline FRGB toFRGB() const noexcept {
      return FRGB(Red / 255.0, Green / 255.0, Blue / 255.0, Alpha / 255.0);
   }
} RGB8;

constexpr RGB8 FRGB::toRGB8() const noexcept {
   return RGB8 {
      uint8_t((Red   >= 1.0f) ? 255 : (Red   < 0.0f ? 0 : Red   * 255.0)),
      uint8_t((Green >= 1.0f) ? 255 : (Green < 0.0f ? 0 : Green * 255.0)),
      uint8_t((Blue  >= 1.0f) ? 255 : (Blue  < 0.0f ? 0 : Blue  * 255.0)),
      uint8_t((Alpha >= 1.0f) ? 255 : (Alpha < 0.0f ? 0 : Alpha * 255.0))
   };
}

constexpr RGB8::RGB8(FRGB frgb) noexcept {
   Red   = uint8_t((frgb.Red   >= 1.0f) ? 255 : (frgb.Red   < 0.0f) ? 0 : frgb.Red   * 255.0);
   Green = uint8_t((frgb.Green >= 1.0f) ? 255 : (frgb.Green < 0.0f) ? 0 : frgb.Green * 255.0);
   Blue  = uint8_t((frgb.Blue  >= 1.0f) ? 255 : (frgb.Blue  < 0.0f) ? 0 : frgb.Blue  * 255.0);
   Alpha = uint8_t((frgb.Alpha >= 1.0f) ? 255 : (frgb.Alpha < 0.0f) ? 0 : frgb.Alpha * 255.0);
}

struct CIEXYZ {
   float X;        // X is a mix of the three CIE RGB curves chosen to be non-negative
   float Y;        // Luminance value from 0 to 1.0
   float Z;        // Z is quasi-equal to blue
   float Alpha;    // Alpha blending value from 0 to 1.0
   constexpr CIEXYZ() noexcept = default;
   constexpr CIEXYZ(float x, float y, float z, float a = 1.0) noexcept
     : X(x), Y(y), Z(z), Alpha(a) { }

   // Convert sRGB (gamma-encoded, 0-1) to CIE XYZ via linear sRGB.

   inline CIEXYZ(const FRGB &RGB) {
      auto to_linear = [](const double V) -> double {
          return (V <= 0.04045) ? V / 12.92 : pow((V + 0.055) / 1.055, 2.4);
      };
      const double lr = to_linear(RGB.Red);
      const double lg = to_linear(RGB.Green);
      const double lb = to_linear(RGB.Blue);
      X = (0.4124564 * lr) + (0.3575761 * lg) + (0.1804375 * lb);
      Y = (0.2126729 * lr) + (0.7151522 * lg) + (0.0721750 * lb);
      Z = (0.0193339 * lr) + (0.1191920 * lg) + (0.9505041 * lb);
      Alpha = RGB.Alpha;
   }

   inline CIEXYZ(const RGB8 &RGB) {
      auto to_linear = [](const double V) -> double {
          return (V <= 0.04045) ? V / 12.92 : pow((V + 0.055) / 1.055, 2.4);
      };
      const double lr = to_linear(RGB.Red / 255.0);
      const double lg = to_linear(RGB.Green / 255.0);
      const double lb = to_linear(RGB.Blue / 255.0);
      X = (0.4124564 * lr) + (0.3575761 * lg) + (0.1804375 * lb);
      Y = (0.2126729 * lr) + (0.7151522 * lg) + (0.0721750 * lb);
      Z = (0.0193339 * lr) + (0.1191920 * lg) + (0.9505041 * lb);
      Alpha = RGB.Alpha / 255.0;
   }

   // Convert CIE XYZ to sRGB via the inverse sRGB matrix and gamma encoding

   inline FRGB toFRGB() const {
      auto linear_to_srgb = [](double V) -> float {
         if (V >= 0.0) return float((V <= 0.0031308) ? 12.92 * V : 1.055 * pow(V, 1.0 / 2.4) - 0.055);
         return float(-1.055 * pow(-V, 1.0 / 2.4) + 0.055);
      };
      double lr =  (3.2404542 * X) - (1.5371385 * Y) - (0.4985314 * Z);
      double lg = (-0.9692660 * X) + (1.8760108 * Y) + (0.0415560 * Z);
      double lb =  (0.0556434 * X) - (0.2040259 * Y) + (1.0572252 * Z);
      return FRGB(linear_to_srgb(lr), linear_to_srgb(lg), linear_to_srgb(lb), float(Alpha));
   }

   inline RGB8 toRGB() const {
      auto frgb = toFRGB();
      return RGB8(frgb.Red * 255, frgb.Green * 255, frgb.Blue * 255, frgb.Alpha * 255);
   }
};

struct RGB16 {
   uint16_t Red;    // Red component value
   uint16_t Green;  // Green component value
   uint16_t Blue;   // Blue component value
   uint16_t Alpha;  // Alpha component value
};

struct RGB32 {
   uint32_t Red;    // Red component value
   uint32_t Green;  // Green component value
   uint32_t Blue;   // Blue component value
   uint32_t Alpha;  // Alpha component value
};

struct RGBPalette {
   int AmtColours;          // Total colours
   struct RGB8 Col[256];    // RGB Palette
};

typedef struct ColourFormat {
   uint8_t RedShift;        // Right shift value for red (15/16 bit formats only)
   uint8_t GreenShift;      // Right shift value for green
   uint8_t BlueShift;       // Right shift value for blue
   uint8_t AlphaShift;      // Right shift value for alpha
   uint8_t RedMask;         // Unshifted mask value for red (ranges from 0x00 to 0xff)
   uint8_t GreenMask;       // Unshifted mask value for green
   uint8_t BlueMask;        // Unshifted mask value for blue
   uint8_t AlphaMask;       // Unshifted mask value for alpha
   uint8_t RedPos;          // Left shift/positional value for red
   uint8_t GreenPos;        // Left shift/positional value for green
   uint8_t BluePos;         // Left shift/positional value for blue
   uint8_t AlphaPos;        // Left shift/positional value for alpha
   uint8_t BitsPerPixel;    // Number of bits per pixel for this format.
} COLOURFORMAT;

struct ClipRectangle {
   int Left;    // Left-most coordinate
   int Top;     // Top coordinate
   int Right;   // Right-most coordinate
   int Bottom;  // Bottom coordinate

   constexpr ClipRectangle() noexcept = default;
   constexpr ClipRectangle(int Value) : Left(Value), Top(Value), Right(Value), Bottom(Value) { }
   constexpr ClipRectangle(int pLeft, int pTop, int pRight, int pBottom) : Left(pLeft), Top(pTop), Right(pRight), Bottom(pBottom) { }
   [[nodiscard]] constexpr int width() const { return Right - Left; }
   [[nodiscard]] constexpr int height() const { return Bottom - Top; }
   constexpr void translate(int pX, int pY) {
      Left   += pX;
      Top    += pY;
      Right  += pX;
      Bottom += pY;
   }
};

struct Edges {
   int Left;    // Left-most coordinate
   int Top;     // Top coordinate
   int Right;   // Right-most coordinate
   int Bottom;  // Bottom coordinate
};

#define AHASH_ACTIVATE 0x725d52c7
#define AHASH_CLEAR 0xce7ed463
#define AHASH_FREEWARNING 0x09a5a3d1
#define AHASH_COPYDATA 0xa7a6c30c
#define AHASH_DATAFEED 0xa6ab005e
#define AHASH_DEACTIVATE 0x0c6900b2
#define AHASH_DRAW 0x6046e885
#define AHASH_FLUSH 0x05b456cd
#define AHASH_FOCUS 0x269d690f
#define AHASH_FREE 0x60ce1d40
#define AHASH_GETKEY 0x603bde11
#define AHASH_DRAGDROP 0x59fb1b5c
#define AHASH_HIDE 0xe3b0779f
#define AHASH_INIT 0x680cc731
#define AHASH_LOCK 0x9f36c619
#define AHASH_LOSTFOCUS 0xb0bab76d
#define AHASH_MOVE 0x8ce6dbac
#define AHASH_MOVETOBACK 0x343f4edb
#define AHASH_MOVETOFRONT 0x3eb5005d
#define AHASH_NEWCHILD 0x8dae9f1d
#define AHASH_NEWOWNER 0x62155f0c
#define AHASH_NEWOBJECT 0xa96da206
#define AHASH_REDO 0x8e9a3422
#define AHASH_QUERY 0x61b0e0a8
#define AHASH_READ 0x4a3b66b2
#define AHASH_RENAME 0xea82e4d4
#define AHASH_RESET 0x9b741eeb
#define AHASH_RESIZE 0x2e3cab45
#define AHASH_SAVEIMAGE 0x1a454349
#define AHASH_SAVETOOBJECT 0xdaa9c534
#define AHASH_SEEK 0x87e791f2
#define AHASH_SETKEY 0x12c2d958
#define AHASH_SHOW 0x636562ba
#define AHASH_TIMER 0x39967fa6
#define AHASH_UNLOCK 0xefbb65e2
#define AHASH_NEXT 0x8f14e8bb
#define AHASH_PREV 0x4ee18a94
#define AHASH_WRITE 0x7a8afad8
#define AHASH_SETFIELD 0xb4e04798
#define AHASH_CLIPBOARD 0xb5bbe3e7
#define AHASH_REFRESH 0x8dbbc14f
#define AHASH_DISABLE 0x79a1d3cd
#define AHASH_ENABLE 0x375b100a
#define AHASH_REDIMENSION 0xab2e92ef
#define AHASH_MOVETOPOINT 0x4906bfe1
#define AHASH_SORT 0xfb3c3eb5
#define AHASH_SAVESETTINGS 0x1f35f58b
#define AHASH_SIGNAL 0xec1cd0b6
#define AHASH_NEWPLACEMENT 0x25d84fc8
#define AHASH_UNDO 0x45e5725e
#define AHASH_FREEPLACEMENT 0x560489a0


typedef AC ACTIONID;

#ifndef __GNUC__
#define __attribute__(a)
#endif

typedef const std::vector<std::pair<std::string, uint32_t>> STRUCTS;
typedef std::map<std::string, std::string, std::less<>> KEYVALUE;

using LOG_CALLBACK = void(*)(CSTRING Header, CSTRING Message, int Depth, int MsgLevel, int LogLevel);

#ifndef STRINGIFY
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#endif

#define MOD_IDL nullptr

#ifdef KOTUKU_STATIC
__export void CloseCore(void);
__export ERR OpenCore(struct OpenInfo *, struct CoreBase **);
#else
__export struct ModHeader ModHeader;
#endif

#ifdef MOD_NAME
#ifdef KOTUKU_STATIC
#define KOTUKU_MOD(init,close,open,expunge,test,IDL,Structures) static struct ModHeader ModHeader(init, close, open, expunge, test, IDL, Structures, TOSTRING(MOD_NAME), TOSTRING(MOD_NAMESPACE));
#else
#define KOTUKU_MOD(init,close,open,expunge,test,IDL,Structures) struct ModHeader ModHeader(init, close, open, expunge, test, IDL, Structures, TOSTRING(MOD_NAME), TOSTRING(MOD_NAMESPACE));
#endif
#define MOD_PATH ("modules:" TOSTRING(MOD_NAME))
#else
#define MOD_NAME nullptr
#endif

namespace kt {

#ifdef PRINTF64I
  #define PF64 "I64d"
#elif PRINTF64_PRID
  #define PF64 PRId64
#else
  #define PF64 "lld"
#endif

// Use DEBUG_BREAK in critical areas where you would want to break in gdb.  This feature will only be compiled
// in to debug builds.

#ifndef NDEBUG
 #ifdef _MSC_VER
  #define DEBUG_BREAK __debugbreak();
 #elif __linux__
  #define DEBUG_BREAK raise(SIGTRAP);
 #else
  #define DEBUG_BREAK
 #endif
#else
 #define DEBUG_BREAK
#endif

template <class T>
constexpr T roundup(T Num, T Alignment) {
   return ((Num + Alignment - 1) / Alignment) * Alignment;
}

[[deprecated]] inline int F2I(double val) noexcept {
   return std::lrint(val);
}

} // namespace

constexpr RESOURCEID RESOURCEID_INHERIT = 1;

#ifdef _LP64
#define FD_PTR64 FD_POINTER
#else
#define FD_PTR64 0
#endif

template <class T>
inline void nextutf8(T *&Str) noexcept {
   if (*Str) {
      for (++Str; (((unsigned char)*Str) & 0xc0) IS 0x80; ++Str);
   }
}

//********************************************************************************************************************

namespace kt {

class FloatRect {
   public:
   double X, Y, Width, Height;
   constexpr FloatRect() noexcept = default;
   constexpr FloatRect(double Value) noexcept : X(Value), Y(Value), Width(Value), Height(Value) { }
   constexpr FloatRect(double pX, double pY, double pWidth, double pHeight) noexcept : X(pX), Y(pY), Width(pWidth), Height(pHeight) { }
   constexpr double left() const noexcept { return X; }
   constexpr double top() const noexcept { return Y; }
   constexpr double right() const noexcept { return X + Width; }
   constexpr double bottom() const noexcept { return Y + Height; }
};

}


struct OpenTag {
   TOI Tag;    // Tag identifier
   union {
      int Int;
      int64_t Int64;
      APTR Pointer;
      CSTRING String;
   } Value;
};

struct OpenInfo {
   std::string Name;                  // Program name
   std::string SystemPath;            // Path to system files
   std::string ModulePath;            // Path to module files
   std::string RootPath;              // Kotuku root directory
   CSTRING *Args;                     // Command-line arguments
   const struct OpenTag * Options;    // Tag-list of additional options.  Typecast to va_list
   OPF     Flags;                     // Client flags indicating the values that have been defined in this structure
   int     MaxDepth;                  // Maximum debug depth
   int     Detail;                    // Debug detail level (0 none - 9 trace)
   int     ArgCount;                  // Total arguments in Args
};

struct ObjectSignal {
   OBJECTPTR Object;    // Reference to an object to monitor.
};

struct ResourceManager {
   CSTRING Name;                                                              // The name of the resource
   ERR (*Free)(struct ResourceRecord &, APTR);                                // A function that will remove the resource's content when terminated
   void (*AddChild)(struct ResourceRecord &, struct ResourceRecord &);        // Optional function for tracking child resources
   void (*RemoveChild)(struct ResourceRecord &, struct ResourceRecord &);     // Optional function to remove tracking of child resources
   bool    CanBlock;                                                          // True if the Free callback might wait on locks, callbacks or external resources
};

struct FunctionField {
   CSTRING  Name;   // Name of the field
   uint32_t Type;   // Type of the field
};

struct Function {
   APTR    Address;                      // Pointer to the function entry point
   CSTRING Name;                         // Name of the function
   const struct FunctionField * Args;    // A list of parameters accepted by the function
};

using ModClose   = void (*)(OBJECTPTR);
using ModInit    = ERR (*)(OBJECTPTR, struct CoreBase*);
using ModOpen    = ERR (*)(OBJECTPTR);
using ModExpunge = ERR (*)(void);
using ModTest    = void (*)(std::string_view, int *, int *);
struct ModHeader {
   MHF     Flags;                                    // Special flags, type of function table wanted from the Core
   CSTRING Definitions;                              // Module definition string, usable by run-time languages such as Tiri
   ERR (*Init)(OBJECTPTR, struct CoreBase *);        // A one-off initialisation routine for when the module is first opened.
   void (*Close)(OBJECTPTR);                         // A function that will be called each time the module is closed.
   ERR (*Open)(OBJECTPTR);                           // A function that will be called each time the module is opened.
   ERR (*Expunge)(void);                             // Reference to an expunge function to terminate the module.
   void (*Test)(std::string_view, int *, int *);     // A function that can run embedded unit tests in development builds.
   CSTRING Name;                                     // Name of the module
   CSTRING Namespace;                                // A reserved system-wide namespace for function names.
   STRUCTS *StructDefs;
   class RootModule *Root;

   ModHeader(ModInit pInit, ModClose pClose, ModOpen pOpen, ModExpunge pExpunge, ModTest pTest,
      CSTRING pDef, STRUCTS *pStructs, CSTRING pName, CSTRING pNamespace) {
      Flags         = MHF::DEFAULT;
      Definitions   = pDef;
      StructDefs    = pStructs;
      Init          = pInit;
      Close         = pClose;
      Open          = pOpen;
      Expunge       = pExpunge;
      Test          = pTest;
      Name          = pName;
      Namespace     = pNamespace;
      Root          = nullptr;
   }
};

struct FieldArray {
   CSTRING  Name;   // The name of the field, e.g. Width
   APTR     GetField; // void GetField(*Object, APTR Result);
   APTR     SetField; // ERR SetField(*Object, APTR Value);
   int64_t  Arg;    // Can be a pointer or an integer value
   uint32_t Flags;  // Special flags that describe the field
  template <class G = APTR, class S = APTR, class T = MAXINT> FieldArray(CSTRING pName, uint32_t pFlags, G pGetField = nullptr, S pSetField = nullptr, T pArg = 0) :
     Name(pName), GetField((APTR)pGetField), SetField((APTR)pSetField), Arg((MAXINT)pArg), Flags(pFlags)
     { }
};

struct FieldDef {
   CSTRING Name;    // The name of the constant.
   int     Value;   // The value of the constant.
   template <class T> FieldDef(CSTRING pName, T pValue) : Name(pName), Value(int(pValue)) { }
};

struct TimeZoneTransition {
   int64_t Instant;             // Unix epoch microseconds at which the transition occurs.
   std::string Abbreviation;    // Short name where available, e.g. GMT, BST, PST.
   int     OffsetBefore;        // UTC offset in seconds before the transition.
   int     OffsetAfter;         // UTC offset in seconds after the transition.
   int     DaylightSaving;      // 1 if the resulting period observes daylight-saving time.
   TimeZoneTransition() : Instant(0), OffsetBefore(0), OffsetAfter(0), DaylightSaving(0) { }
};

struct TimeZoneInfo {
   std::string ZoneID;                            // Preferred public ID, e.g. Europe/London or UTC.
   std::string NativeID;                          // Host-native ID, e.g. GMT Standard Time on Windows.
   std::string Source;                            // "tzif", "win32", or "utc".
   std::string DataPath;                          // TZif path on Linux, otherwise empty.
   std::string Version;                           // TZDB/source version if available, otherwise empty.
   kt::vector<TimeZoneTransition> Transitions;    // Transitions available for the requested range.
   int     BaseOffset;                            // Standard UTC offset in seconds.
   int16_t StartYear;                             // Inclusive requested start year.
   int16_t EndYear;                               // Inclusive requested end year.
   int8_t  IsLocal;                               // 1 if ZoneID requested the local system zone.
   int8_t  IsFallback;                            // 1 if UTC fallback was used.
   TimeZoneInfo() : BaseOffset(0), StartYear(0), EndYear(0), IsLocal(0), IsFallback(0) { }
   TimeZoneInfo(int) : TimeZoneInfo() { }
};

struct SystemState {
   CSTRING Platform;                    // String-based field indicating the user's platform.  Currently returns Native, Windows, OSX or Linux.
   CSTRING IDL;                         // The Core module's compressed IDL string
   const struct OpenInfo * OpenInfo;    // The OpenInfo structure originally used to initialise the system
   HOSTHANDLE ConsoleFD;                // Internal
   CONTYPE ConsoleType;                 // The console type for stdout and stderr, if any
   int16_t Stage;                       // The current operating stage.  -1 = Initialising, 0 indicates normal operating status; 1 means that the program is shutting down; 2 indicates a program restart; 3 is for mode switches.
   uint8_t ReleaseBuild;                // 1 = Release build, 0 = Debug build
   uint8_t StaticBuild;                 // 1 = Static build, 0 = Dynamic build
};

struct Unit {
   double   Value;  // The unit value.
   uint32_t Type;   // Additional type information
   constexpr Unit(double pValue, int pType = FD_DOUBLE) : Value(pValue), Type(pType) { }
   constexpr Unit() : Value(0), Type(0) { }
   explicit Unit(std::string_view String) { read(String); }
   constexpr operator double() const { return Value; }
   constexpr void set(const double pValue) { Value = pValue; }
   constexpr bool scaled() { return (Type & FD_SCALED) ? true : false; }
   inline void read(std::string_view String) {
      const auto start = String.find_first_not_of(" \n\r\t");
      if (start != std::string::npos) String.remove_prefix(start);
      if (String.starts_with('+')) String.remove_prefix(1);
      auto [ end, error ] = std::from_chars(String.data(), String.data() + String.size(), Value);
      if (error != std::errc()) { Value = 0; return; }

      String = String.substr(end - String.data());
      if (String.starts_with("%")) { Value *= 0.01; Type = FD_DOUBLE|FD_SCALED; }
      else Type = FD_DOUBLE;
   }
};

struct ActionArray {
   APTR Routine;    // Pointer to the function entry point
   AC   ActionCode; // Action identifier
  template <class T> ActionArray(AC pID, T pRoutine) : Routine((APTR)pRoutine), ActionCode(pID) { }
};

struct MethodEntry {
   AC      MethodID;                     // Unique method identifier
   APTR    Routine;                      // The method entry point, defined as ERR (*Routine)(OBJECTPTR, APTR);
   CSTRING Name;                         // Name of the method
   const struct FunctionField * Args;    // List of parameters accepted by the method
   int     Size;                         // Total byte-size of all accepted parameters when they are assembled as a C structure.
   MethodEntry() : MethodID(AC::NIL), Routine(nullptr), Name(nullptr), Args(nullptr), Size(0) { }
   MethodEntry(AC pID, APTR pRoutine, CSTRING pName, const struct FunctionField *pArgs = nullptr, int pSize = 0) :
      MethodID(pID), Routine(pRoutine), Name(pName), Args(pArgs), Size(pSize) { }
};

struct ActionTable {
   uint32_t Hash;                        // Hash of the action name.
   int      Size;                        // Byte-size of the structure for this action.
   CSTRING  Name;                        // Name of the action.
   const struct FunctionField * Args;    // List of fields that are passed to this action.
};

struct ChildEntry {
   OBJECTID ObjectID;    // Object ID
   CLASSID  ClassID;     // The class ID of the referenced object.
};

struct Message {
   int64_t Time;    // A timestamp acquired from ~Core.PreciseTime() when the message was first passed to ~Core.SendMessage().
   int     UID;     // A unique identifier automatically created by ~Core.SendMessage().
   MSGID   Type;    // A message type identifier as defined by the client.
   int     Size;    // The byte-size of the message data, or zero if no data is provided.
};

typedef struct MemInfo {
   APTR     Start;       // The starting address of the memory block (does not apply to shared blocks).
   uint32_t Size;        // The size of the memory block.
   MEM      Flags;       // The type of memory.
   MEMORYID MemoryID;    // The unique resource ID for this block.
} MEMINFO;

struct MsgHandler {
   struct MsgHandler * Prev;    // Previous message handler in the chain.
   struct MsgHandler * Next;    // Next message handler in the chain.
   FUNCTION Function;           // Call this function to handle the message.
   MSGID    MsgType;            // Type of message being filtered.
};

struct CacheFile {
   int64_t Timestamp;    // The file's last-modified timestamp.
   int64_t Size;         // Byte size of the cached data.
   int64_t LastUse;      // The last time that this file was requested.
   CSTRING Path;         // Pointer to the resolved file path.
   APTR    Data;         // Pointer to the cached data.
};

struct CompressionFeedback {
   FDB     FeedbackID;      // Set to one of the FDB event indicators
   int     Index;           // Index of the current file
   CSTRING Path;            // Name of the current file/path in the archive
   CSTRING Dest;            // Destination file/path during decompression
   int64_t Progress;        // Progress indicator (byte position for the file being de/compressed).
   int64_t OriginalSize;    // Original size of the file
   int64_t CompressedSize;  // Compressed size of the file
   int16_t Year;            // Year of the original file's datestamp.
   int16_t Month;           // Month of the original file's datestamp.
   int16_t Day;             // Day of the original file's datestamp.
   int16_t Hour;            // Hour of the original file's datestamp.
   int16_t Minute;          // Minute of the original file's datestamp.
   int16_t Second;          // Second of the original file's datestamp.
   CompressionFeedback() : FeedbackID(FDB::NIL), Index(0), Path(nullptr), Dest(nullptr),
      Progress(0), OriginalSize(0), CompressedSize(0),
      Year(0), Month(0), Day(0), Hour(0), Minute(0), Second(0) { }

   CompressionFeedback(FDB pFeedback, int pIndex, CSTRING pPath, CSTRING pDest) :
      FeedbackID(pFeedback), Index(pIndex), Path(pPath), Dest(pDest),
      Progress(0), OriginalSize(0), CompressedSize(0),
      Year(0), Month(0), Day(0), Hour(0), Minute(0), Second(0) { }
};

struct CompressedItem {
   int64_t OriginalSize;            // Original size of the file
   int64_t CompressedSize;          // Compressed size of the file
   struct CompressedItem * Next;    // Used only if this is a linked-list.
   CSTRING Path;                    // Path to the file (includes folder prefixes).  Archived folders will include the trailing slash.
   PERMIT  Permissions;             // Original permissions - see PERMIT flags.
   int     UserID;                  // Original user ID
   int     GroupID;                 // Original group ID
   int     OthersID;                // Original others ID
   FL      Flags;                   // FL flags
   struct DateTime Created;         // Date and time of the file's creation.
   struct DateTime Modified;        // Date and time last modified.
    ankerl::unordered_dense::map<std::string, std::string> *Tags;
};

struct FileInfo {
   int64_t Size;              // The size of the file's content.
   int64_t Timestamp;         // 64-bit time stamp - usable only for comparison (e.g. sorting).
   struct FileInfo * Next;    // Next structure in the list, or NULL.
   std::string Name;          // The name of the file.
   RDF     Flags;             // Additional flags to describe the file.
   PERMIT  Permissions;       // Standard permission flags.
   int     UserID;            // User  ID (Unix systems only).
   int     GroupID;           // Group ID (Unix systems only).
   struct DateTime Created;   // The date/time of the file's creation.
   struct DateTime Modified;  // The date/time of the last file modification.
   struct fi_hash {
      using is_transparent = void;
      [[nodiscard]] size_t operator()(std::string_view Value) const noexcept { return std::hash<std::string_view>{}(Value); }
   };

   struct fi_equal {
      using is_transparent = void;
      [[nodiscard]] bool operator()(const std::string_view &Lhs, const std::string_view &Rhs) const noexcept { return Lhs IS Rhs; }
   };

   using TagMap = ankerl::unordered_dense::map<std::string, std::string, fi_hash, fi_equal>;

   TagMap *Tags;

   inline void clearTags() { if (Tags) { delete Tags; Tags = nullptr; } }

   inline TagMap * getTags() {
      if (not Tags) {
         Tags = new (std::nothrow) TagMap();
         if (not Tags) return nullptr;
      }
      return Tags;
   }
};

struct DirInfo {
   struct FileInfo * Info;    // Pointer to a FileInfo structure
   #ifdef PRV_FILE
   APTR   Driver;
   APTR   prvHandle;        // Directory handle.  If virtual, may store a private data address
   STRING prvPath;          // Original folder location string
   STRING prvResolvedPath;  // Resolved folder location
   RDF    prvFlags;         // OpenFolder() RDF flags
   int    prvTotal;         // Total number of items in the folder
   uint32_t prvVirtualID;   // Unique ID (name hash) for a virtual device
   union {
      int prvIndex;         // Current index within the folder when scanning
      APTR prvIndexPtr;
   };
   int16_t   prvResolveLen;    // Byte length of ResolvedPath
   #endif
};

struct FileFeedback {
   int64_t Size;        // Size of the file
   int64_t Position;    // Current seek position within the file if moving or copying
   STRING  Path;        // Path to the file
   STRING  Dest;        // Destination file/path if moving or copying
   FBK     FeedbackID;  // Set to one of the FBK values
   char    Reserved[32]; // Reserved in case of future expansion
  FileFeedback() : Size(0), Position(0), Path(nullptr), Dest(nullptr), FeedbackID(FBK::NIL) { }
};

struct Field {
   MAXINT   Arg;                                                             // An option to complement the field type.  Can be a pointer or an integer value
   ERR (*GetValue)(APTR, APTR);                                              // A virtual function that will retrieve the value for this field
   APTR     SetValue;                                                        // A virtual function that will set the value for this field
   ERR (*WriteValue)(OBJECTPTR, struct Field *, int, const void *, int);     // An internal function for writing to this field
   CSTRING  Name;                                                            // The English name for the field, e.g. Width
   uint32_t FieldID;                                                         // 32-bit hash from fieldhash(). Represented by FID constants, e.g. FID_Width
   uint16_t Offset;                                                          // Field offset within the object
   uint16_t Index;                                                           // Field array index
   uint32_t Flags;                                                           // Special flags that describe the field
   inline bool readable() { return (Flags & FD_READ) ? true : false; }
   inline bool writeable() { return (Flags & (FD_WRITE|FD_INIT)) ? true : false; }
};

struct ClassRecord {
   CLASSID ClassID;          // Unique class identifier (hash of Name)
   CLASSID ParentID;         // Parent class ID if this is a derived class
   CCF     Category;         // Assigned category
   std::string Name;         // Name of the class
   std::string Path;         // Path to the class file
   std::string Extension;    // Wildcards for matching by file extension, e.g. jpeg|jpg
   std::string Header;       // File identification instruction, e.g. [0:$89504e470d0a1a0a]
   std::string Icon;         // Icon reference in group/name format
   std::string Description;  // File description
};

struct ScriptArg { // For use with sc::Exec
   CSTRING Name;
   uint32_t Type;
   union {
      APTR    Address;
      int     Int;
      int64_t Int64;
      double  Double;
   };

   ScriptArg(CSTRING pName, OBJECTPTR pValue, uint32_t pType = FD_OBJECTPTR) : Name(pName), Type(pType), Address((APTR)pValue) { }
   ScriptArg(CSTRING pName, std::string &pValue, uint32_t pType = FD_STRING) : Name(pName), Type(pType), Address((APTR)pValue.data()) { }
   ScriptArg(CSTRING pName, const std::string &pValue, uint32_t pType = FD_STRING) : Name(pName), Type(pType), Address((APTR)pValue.data()) { }
   ScriptArg(CSTRING pName, CSTRING pValue, uint32_t pType = FD_STRING) : Name(pName), Type(pType), Address((APTR)pValue) { }
   ScriptArg(CSTRING pName, APTR pValue, uint32_t pType = FD_PTR) : Name(pName), Type(pType), Address(pValue) { }
   ScriptArg(CSTRING pName, int pValue, uint32_t pType = FD_INT) : Name(pName), Type(pType), Int(pValue) { }
   ScriptArg(CSTRING pName, uint32_t pValue, uint32_t pType = FD_INT) : Name(pName), Type(pType), Int(pValue) { }
   ScriptArg(CSTRING pName, int64_t pValue, uint32_t pType = FD_INT64) : Name(pName), Type(pType), Int64(pValue) { }
   ScriptArg(CSTRING pName, double pValue, uint32_t pType = FD_DOUBLE) : Name(pName), Type(pType), Double(pValue) { }
};

#ifdef KOTUKU_STATIC
#define JUMPTABLE_CORE [[maybe_unused]] static struct CoreBase *CoreBase = nullptr;
#else
#define JUMPTABLE_CORE struct CoreBase *CoreBase = nullptr;
#endif

struct CoreBase {
#ifndef KOTUKU_STATIC
   ERR (*_Action)(AC Action, OBJECTPTR Object, APTR Parameters);
   void (*_ActionList)(struct ActionTable **Actions, int *Size);
   ERR (*_DeleteFile)(const std::string_view &Path, FUNCTION *Callback);
   CSTRING (*_ResolveClassID)(CLASSID ID);
   int (*_AllocateID)(IDTYPE Type);
   ERR (*_AllocMemory)(int64_t Size, MEM Flags, APTR *Address);
   ERR (*_AccessObject)(OBJECTID Object, int MilliSeconds, OBJECTPTR *Result);
   ERR (*_CheckAction)(OBJECTPTR Object, AC Action);
   ERR (*_CheckResourceExists)(RESOURCEID ID);
   ERR (*_InitObject)(OBJECTPTR Object);
   ERR (*_VirtualVolume)(const std::string_view &Name, ...);
   OBJECTPTR (*_CurrentContext)(void);
   void (*_SetLogCallback)(APTR Callback, int DepthLimit, int LogLimit);
   int (*_AdjustLogLevel)(int Delta);
   ERR (*_ReadFileToBuffer)(const std::string_view &Path, APTR Buffer, int BufferSize, int *Result);
   ERR (*_FindObject)(const std::string_view &Name, CLASSID ClassID, OBJECTID *ObjectID);
   objMetaClass * (*_FindClass)(CLASSID ClassID);
   ERR (*_AnalysePath)(const std::string_view &Path, LOC *Type);
   ERR (*_FreeResource)(RESOURCEID ID);
   CLASSID (*_GetClassID)(OBJECTID Object);
   OBJECTID (*_GetOwnerID)(OBJECTID Object);
   ERR (*_CompareFilePaths)(const std::string_view &PathA, const std::string_view &PathB);
   const struct SystemState * (*_GetSystemState)(void);
   ERR (*_ListChildren)(OBJECTID Object, kt::vector<ChildEntry> *List);
   ERR (*_RegisterFD)(HOSTHANDLE FD, RFD Flags, void (*Routine)(HOSTHANDLE, APTR) , APTR Data);
   ERR (*_ResolvePath)(const std::string_view &Path, RSF Flags, std::string *Result);
   ERR (*_MemoryInfo)(MEMORYID ID, struct MemInfo *MemInfo, int Size);
   ERR (*_TrackResource)(RESOURCEID ResourceID, APTR Address, RESOURCEID OwnerID, struct ResourceManager *Manager);
   ERR (*_NewObject)(CLASSID ClassID, NF Flags, OBJECTPTR *Object);
   void (*_NotifySubscribers)(OBJECTPTR Object, AC Action, APTR Args, ERR Error);
   ERR (*_CopyFile)(const std::string_view &Source, const std::string_view &Dest, FUNCTION *Callback);
   ERR (*_ProcessMessages)(PMF Flags, int TimeOut);
   ERR (*_IdentifyFile)(const std::string_view &Path, CLASSID Filter, CLASSID *Class, CLASSID *SubClass);
   ERR (*_ReallocMemory)(APTR Memory, uint32_t Size, APTR *Address);
   CLASSID (*_ResolveClassName)(const std::string_view &Name);
   ERR (*_SendMessage)(MSGID Type, MSF Flags, APTR Data, int Size);
   ERR (*_SetOwner)(OBJECTPTR Object, OBJECTPTR Owner);
   ERR (*_ProtectMemory)(APTR Address, MEM Flags);
   void (*_SetObjectContext)(OBJECTPTR Object, struct Field *Field, AC ActionID);
   CSTRING (*_FieldName)(uint32_t FieldID);
   ERR (*_ScanDir)(struct DirInfo *Info);
   ERR (*_SetName)(OBJECTPTR Object, const std::string_view &Name);
   void (*_LogReturn)(void);
   ERR (*_SubscribeAction)(OBJECTPTR Object, AC Action, FUNCTION *Callback);
   ERR (*_SubscribeEvent)(int64_t Event, FUNCTION *Callback, APTR *Handle);
   ERR (*_SubscribeTimer)(double Interval, FUNCTION *Callback, APTR *Subscription);
   ERR (*_UpdateTimer)(APTR Subscription, double Interval);
   ERR (*_UnsubscribeAction)(OBJECTPTR Object, AC Action);
   void (*_UnsubscribeEvent)(APTR Handle);
   ERR (*_BroadcastEvent)(APTR Event, int EventSize);
   ERR (*_WaitTime)(double Seconds);
   int64_t (*_GetEventID)(EVG Group, const std::string_view &SubGroup, const std::string_view &Event);
   uint32_t (*_GenCRC32)(uint32_t CRC, APTR Data, uint32_t Length);
   int64_t (*_GetResource)(RES Resource);
   int64_t (*_SetResource)(RES Resource, int64_t Value);
   ERR (*_ScanMessages)(int *Handle, MSGID Type, APTR Buffer, int Size);
   ERR (*_WaitForObjects)(PMF Flags, int TimeOut, struct ObjectSignal *ObjectSignals);
   void (*_UnloadFile)(struct CacheFile *Cache);
   ERR (*_CreateFolder)(const std::string_view &Path, PERMIT Permissions);
   ERR (*_LoadFile)(const std::string_view &Path, LDF Flags, struct CacheFile **Cache);
   ERR (*_SetVolume)(const std::string_view &Name, const std::string_view &Path, const std::string_view &Icon, const std::string_view &Label, const std::string_view &Device, VOLUME Flags);
   ERR (*_DeleteVolume)(const std::string_view &Name);
   ERR (*_MoveFile)(const std::string_view &Source, const std::string_view &Dest, FUNCTION *Callback);
   ERR (*_UpdateMessage)(int Message, MSGID Type, APTR Data, int Size);
   ERR (*_AddMsgHandler)(MSGID MsgType, FUNCTION *Routine, struct MsgHandler **Handle);
   ERR (*_QueueAction)(AC Action, OBJECTID Object, APTR Args);
   int64_t (*_PreciseTime)(void);
   ERR (*_OpenDir)(const std::string_view &Path, RDF Flags, struct DirInfo **Info);
   OBJECTPTR (*_GetObjectPtr)(OBJECTID Object);
   struct Field * (*_FindField)(OBJECTPTR Object, uint32_t FieldID, OBJECTPTR *Target);
   CSTRING (*_GetErrorMsg)(ERR Error);
   struct Message * (*_GetActionMsg)(AC Action);
   ERR (*_FuncError)(CSTRING Header, ERR Error);
   ERR (*_LockObject)(OBJECTPTR Object, int MilliSeconds);
   void (*_ReleaseObject)(OBJECTPTR Object);
   ERR (*_AsyncAction)(AC Action, OBJECTPTR Object, APTR Args, FUNCTION *Callback);
   ERR (*_AddInfoTag)(struct FileInfo *Info, const std::string_view &Name, const std::string_view &Value);
   void (*_SetDefaultPermissions)(int User, int Group, PERMIT Permissions);
   void (*_VLogF)(VLF Flags, const char *Header, const char *Message, va_list Args);
   ERR (*_ReadInfoTag)(struct FileInfo *Info, const std::string_view &Name, std::string_view *Value);
   ERR (*_SetResourcePath)(RP PathType, const std::string_view &Path);
   objTask * (*_CurrentTask)(void);
   CSTRING (*_ResolveGroupID)(int Group);
   CSTRING (*_ResolveUserID)(int User);
   ERR (*_CreateLink)(const std::string_view &From, const std::string_view &To);
   OBJECTPTR (*_ParentContext)(void);
   ERR (*_WakeThread)(int Thread, int Stop);
   ERR (*_AsyncCancel)(kt::vector<OBJECTID> &Objects);
   int (*_AsyncPending)(OBJECTID Object);
   ERR (*_AsyncWait)(kt::vector<OBJECTID> &Objects, int TimeOut);
   ERR (*_ClassDatabase)(kt::vector<ClassRecord *> *Classes);
#endif // KOTUKU_STATIC
};

#if !defined(KOTUKU_STATIC) and !defined(PRV_CORE_MODULE)
extern struct CoreBase *CoreBase;
inline ERR Action(AC Action, OBJECTPTR Object, APTR Parameters) { return CoreBase->_Action(Action,Object,Parameters); }
inline void ActionList(struct ActionTable **Actions, int *Size) { return CoreBase->_ActionList(Actions,Size); }
inline ERR DeleteFile(const std::string_view &Path, FUNCTION *Callback) { return CoreBase->_DeleteFile(Path,Callback); }
inline CSTRING ResolveClassID(CLASSID ID) { return CoreBase->_ResolveClassID(ID); }
inline int AllocateID(IDTYPE Type) { return CoreBase->_AllocateID(Type); }
inline ERR AllocMemory(int64_t Size, MEM Flags, APTR *Address) { return CoreBase->_AllocMemory(Size,Flags,Address); }
inline ERR AccessObject(OBJECTID Object, int MilliSeconds, OBJECTPTR *Result) { return CoreBase->_AccessObject(Object,MilliSeconds,Result); }
inline ERR CheckAction(OBJECTPTR Object, AC Action) { return CoreBase->_CheckAction(Object,Action); }
inline ERR CheckResourceExists(RESOURCEID ID) { return CoreBase->_CheckResourceExists(ID); }
inline ERR InitObject(OBJECTPTR Object) { return CoreBase->_InitObject(Object); }
template<class... Args> ERR VirtualVolume(const std::string_view &Name, Args... Tags) { return CoreBase->_VirtualVolume(Name,Tags...); }
inline OBJECTPTR CurrentContext(void) { return CoreBase->_CurrentContext(); }
inline void SetLogCallback(APTR Callback, int DepthLimit, int LogLimit) { return CoreBase->_SetLogCallback(Callback,DepthLimit,LogLimit); }
inline int AdjustLogLevel(int Delta) { return CoreBase->_AdjustLogLevel(Delta); }
inline ERR ReadFileToBuffer(const std::string_view &Path, APTR Buffer, int BufferSize, int *Result) { return CoreBase->_ReadFileToBuffer(Path,Buffer,BufferSize,Result); }
inline ERR FindObject(const std::string_view &Name, CLASSID ClassID, OBJECTID *ObjectID) { return CoreBase->_FindObject(Name,ClassID,ObjectID); }
inline objMetaClass * FindClass(CLASSID ClassID) { return CoreBase->_FindClass(ClassID); }
inline ERR AnalysePath(const std::string_view &Path, LOC *Type) { return CoreBase->_AnalysePath(Path,Type); }
inline ERR FreeResource(RESOURCEID ID) { return CoreBase->_FreeResource(ID); }
inline CLASSID GetClassID(OBJECTID Object) { return CoreBase->_GetClassID(Object); }
inline OBJECTID GetOwnerID(OBJECTID Object) { return CoreBase->_GetOwnerID(Object); }
inline ERR CompareFilePaths(const std::string_view &PathA, const std::string_view &PathB) { return CoreBase->_CompareFilePaths(PathA,PathB); }
inline const struct SystemState * GetSystemState(void) { return CoreBase->_GetSystemState(); }
inline ERR ListChildren(OBJECTID Object, kt::vector<ChildEntry> *List) { return CoreBase->_ListChildren(Object,List); }
inline ERR RegisterFD(HOSTHANDLE FD, RFD Flags, void (*Routine)(HOSTHANDLE, APTR) , APTR Data) { return CoreBase->_RegisterFD(FD,Flags,Routine,Data); }
inline ERR ResolvePath(const std::string_view &Path, RSF Flags, std::string *Result) { return CoreBase->_ResolvePath(Path,Flags,Result); }
inline ERR MemoryInfo(MEMORYID ID, struct MemInfo *MemInfo, int Size) { return CoreBase->_MemoryInfo(ID,MemInfo,Size); }
inline ERR TrackResource(RESOURCEID ResourceID, APTR Address, RESOURCEID OwnerID, struct ResourceManager *Manager) { return CoreBase->_TrackResource(ResourceID,Address,OwnerID,Manager); }
inline ERR NewObject(CLASSID ClassID, NF Flags, OBJECTPTR *Object) { return CoreBase->_NewObject(ClassID,Flags,Object); }
inline void NotifySubscribers(OBJECTPTR Object, AC Action, APTR Args, ERR Error) { return CoreBase->_NotifySubscribers(Object,Action,Args,Error); }
inline ERR CopyFile(const std::string_view &Source, const std::string_view &Dest, FUNCTION *Callback) { return CoreBase->_CopyFile(Source,Dest,Callback); }
inline ERR ProcessMessages(PMF Flags, int TimeOut) { return CoreBase->_ProcessMessages(Flags,TimeOut); }
inline ERR IdentifyFile(const std::string_view &Path, CLASSID Filter, CLASSID *Class, CLASSID *SubClass) { return CoreBase->_IdentifyFile(Path,Filter,Class,SubClass); }
inline ERR ReallocMemory(APTR Memory, uint32_t Size, APTR *Address) { return CoreBase->_ReallocMemory(Memory,Size,Address); }
inline CLASSID ResolveClassName(const std::string_view &Name) { return CoreBase->_ResolveClassName(Name); }
inline ERR SendMessage(MSGID Type, MSF Flags, APTR Data, int Size) { return CoreBase->_SendMessage(Type,Flags,Data,Size); }
inline ERR SetOwner(OBJECTPTR Object, OBJECTPTR Owner) { return CoreBase->_SetOwner(Object,Owner); }
inline ERR ProtectMemory(APTR Address, MEM Flags) { return CoreBase->_ProtectMemory(Address,Flags); }
inline void SetObjectContext(OBJECTPTR Object, struct Field *Field, AC ActionID) { return CoreBase->_SetObjectContext(Object,Field,ActionID); }
inline CSTRING FieldName(uint32_t FieldID) { return CoreBase->_FieldName(FieldID); }
inline ERR ScanDir(struct DirInfo *Info) { return CoreBase->_ScanDir(Info); }
inline ERR SetName(OBJECTPTR Object, const std::string_view &Name) { return CoreBase->_SetName(Object,Name); }
inline void LogReturn(void) { return CoreBase->_LogReturn(); }
inline ERR SubscribeAction(OBJECTPTR Object, AC Action, FUNCTION *Callback) { return CoreBase->_SubscribeAction(Object,Action,Callback); }
inline ERR SubscribeEvent(int64_t Event, FUNCTION *Callback, APTR *Handle) { return CoreBase->_SubscribeEvent(Event,Callback,Handle); }
inline ERR SubscribeTimer(double Interval, FUNCTION *Callback, APTR *Subscription) { return CoreBase->_SubscribeTimer(Interval,Callback,Subscription); }
inline ERR UpdateTimer(APTR Subscription, double Interval) { return CoreBase->_UpdateTimer(Subscription,Interval); }
inline ERR UnsubscribeAction(OBJECTPTR Object, AC Action) { return CoreBase->_UnsubscribeAction(Object,Action); }
inline void UnsubscribeEvent(APTR Handle) { return CoreBase->_UnsubscribeEvent(Handle); }
inline ERR BroadcastEvent(APTR Event, int EventSize) { return CoreBase->_BroadcastEvent(Event,EventSize); }
inline ERR WaitTime(double Seconds) { return CoreBase->_WaitTime(Seconds); }
inline int64_t GetEventID(EVG Group, const std::string_view &SubGroup, const std::string_view &Event) { return CoreBase->_GetEventID(Group,SubGroup,Event); }
inline uint32_t GenCRC32(uint32_t CRC, APTR Data, uint32_t Length) { return CoreBase->_GenCRC32(CRC,Data,Length); }
inline int64_t GetResource(RES Resource) { return CoreBase->_GetResource(Resource); }
inline int64_t SetResource(RES Resource, int64_t Value) { return CoreBase->_SetResource(Resource,Value); }
inline ERR ScanMessages(int *Handle, MSGID Type, APTR Buffer, int Size) { return CoreBase->_ScanMessages(Handle,Type,Buffer,Size); }
inline ERR WaitForObjects(PMF Flags, int TimeOut, struct ObjectSignal *ObjectSignals) { return CoreBase->_WaitForObjects(Flags,TimeOut,ObjectSignals); }
inline void UnloadFile(struct CacheFile *Cache) { return CoreBase->_UnloadFile(Cache); }
inline ERR CreateFolder(const std::string_view &Path, PERMIT Permissions) { return CoreBase->_CreateFolder(Path,Permissions); }
inline ERR LoadFile(const std::string_view &Path, LDF Flags, struct CacheFile **Cache) { return CoreBase->_LoadFile(Path,Flags,Cache); }
inline ERR SetVolume(const std::string_view &Name, const std::string_view &Path, const std::string_view &Icon, const std::string_view &Label, const std::string_view &Device, VOLUME Flags) { return CoreBase->_SetVolume(Name,Path,Icon,Label,Device,Flags); }
inline ERR DeleteVolume(const std::string_view &Name) { return CoreBase->_DeleteVolume(Name); }
inline ERR MoveFile(const std::string_view &Source, const std::string_view &Dest, FUNCTION *Callback) { return CoreBase->_MoveFile(Source,Dest,Callback); }
inline ERR UpdateMessage(int Message, MSGID Type, APTR Data, int Size) { return CoreBase->_UpdateMessage(Message,Type,Data,Size); }
inline ERR AddMsgHandler(MSGID MsgType, FUNCTION *Routine, struct MsgHandler **Handle) { return CoreBase->_AddMsgHandler(MsgType,Routine,Handle); }
inline ERR QueueAction(AC Action, OBJECTID Object, APTR Args) { return CoreBase->_QueueAction(Action,Object,Args); }
inline int64_t PreciseTime(void) { return CoreBase->_PreciseTime(); }
inline ERR OpenDir(const std::string_view &Path, RDF Flags, struct DirInfo **Info) { return CoreBase->_OpenDir(Path,Flags,Info); }
inline OBJECTPTR GetObjectPtr(OBJECTID Object) { return CoreBase->_GetObjectPtr(Object); }
inline struct Field * FindField(OBJECTPTR Object, uint32_t FieldID, OBJECTPTR *Target) { return CoreBase->_FindField(Object,FieldID,Target); }
inline CSTRING GetErrorMsg(ERR Error) { return CoreBase->_GetErrorMsg(Error); }
inline struct Message * GetActionMsg(AC Action) { return CoreBase->_GetActionMsg(Action); }
inline ERR FuncError(CSTRING Header, ERR Error) { return CoreBase->_FuncError(Header,Error); }
inline ERR LockObject(OBJECTPTR Object, int MilliSeconds) { return CoreBase->_LockObject(Object,MilliSeconds); }
inline void ReleaseObject(OBJECTPTR Object) { return CoreBase->_ReleaseObject(Object); }
inline ERR AsyncAction(AC Action, OBJECTPTR Object, APTR Args, FUNCTION *Callback) { return CoreBase->_AsyncAction(Action,Object,Args,Callback); }
inline ERR AddInfoTag(struct FileInfo *Info, const std::string_view &Name, const std::string_view &Value) { return CoreBase->_AddInfoTag(Info,Name,Value); }
inline void SetDefaultPermissions(int User, int Group, PERMIT Permissions) { return CoreBase->_SetDefaultPermissions(User,Group,Permissions); }
inline void VLogF(VLF Flags, const char *Header, const char *Message, va_list Args) { return CoreBase->_VLogF(Flags,Header,Message,Args); }
inline ERR ReadInfoTag(struct FileInfo *Info, const std::string_view &Name, std::string_view *Value) { return CoreBase->_ReadInfoTag(Info,Name,Value); }
inline ERR SetResourcePath(RP PathType, const std::string_view &Path) { return CoreBase->_SetResourcePath(PathType,Path); }
inline objTask * CurrentTask(void) { return CoreBase->_CurrentTask(); }
inline CSTRING ResolveGroupID(int Group) { return CoreBase->_ResolveGroupID(Group); }
inline CSTRING ResolveUserID(int User) { return CoreBase->_ResolveUserID(User); }
inline ERR CreateLink(const std::string_view &From, const std::string_view &To) { return CoreBase->_CreateLink(From,To); }
inline OBJECTPTR ParentContext(void) { return CoreBase->_ParentContext(); }
inline ERR WakeThread(int Thread, int Stop) { return CoreBase->_WakeThread(Thread,Stop); }
inline ERR AsyncCancel(kt::vector<OBJECTID> &Objects) { return CoreBase->_AsyncCancel(Objects); }
inline int AsyncPending(OBJECTID Object) { return CoreBase->_AsyncPending(Object); }
inline ERR AsyncWait(kt::vector<OBJECTID> &Objects, int TimeOut) { return CoreBase->_AsyncWait(Objects,TimeOut); }
inline ERR ClassDatabase(kt::vector<ClassRecord *> *Classes) { return CoreBase->_ClassDatabase(Classes); }
#else
extern "C" ERR Action(AC Action, OBJECTPTR Object, APTR Parameters);
extern "C" void ActionList(struct ActionTable **Actions, int *Size);
extern "C" ERR DeleteFile(const std::string_view &Path, FUNCTION *Callback);
extern "C" CSTRING ResolveClassID(CLASSID ID);
extern "C" int AllocateID(IDTYPE Type);
extern "C" ERR AllocMemory(int64_t Size, MEM Flags, APTR *Address);
extern "C" ERR AccessObject(OBJECTID Object, int MilliSeconds, OBJECTPTR *Result);
extern "C" ERR CheckAction(OBJECTPTR Object, AC Action);
extern "C" ERR CheckResourceExists(RESOURCEID ID);
extern "C" ERR InitObject(OBJECTPTR Object);
extern "C" OBJECTPTR CurrentContext(void);
extern "C" void SetLogCallback(APTR Callback, int DepthLimit, int LogLimit);
extern "C" int AdjustLogLevel(int Delta);
extern "C" ERR ReadFileToBuffer(const std::string_view &Path, APTR Buffer, int BufferSize, int *Result);
extern "C" ERR FindObject(const std::string_view &Name, CLASSID ClassID, OBJECTID *ObjectID);
extern "C" objMetaClass * FindClass(CLASSID ClassID);
extern "C" ERR AnalysePath(const std::string_view &Path, LOC *Type);
extern "C" ERR FreeResource(RESOURCEID ID);
extern "C" CLASSID GetClassID(OBJECTID Object);
extern "C" OBJECTID GetOwnerID(OBJECTID Object);
extern "C" ERR CompareFilePaths(const std::string_view &PathA, const std::string_view &PathB);
extern "C" const struct SystemState * GetSystemState(void);
extern "C" ERR ListChildren(OBJECTID Object, kt::vector<ChildEntry> *List);
extern "C" ERR RegisterFD(HOSTHANDLE FD, RFD Flags, void (*Routine)(HOSTHANDLE, APTR) , APTR Data);
extern "C" ERR ResolvePath(const std::string_view &Path, RSF Flags, std::string *Result);
extern "C" ERR MemoryInfo(MEMORYID ID, struct MemInfo *MemInfo, int Size);
extern "C" ERR TrackResource(RESOURCEID ResourceID, APTR Address, RESOURCEID OwnerID, struct ResourceManager *Manager);
extern "C" ERR NewObject(CLASSID ClassID, NF Flags, OBJECTPTR *Object);
extern "C" void NotifySubscribers(OBJECTPTR Object, AC Action, APTR Args, ERR Error);
extern "C" ERR CopyFile(const std::string_view &Source, const std::string_view &Dest, FUNCTION *Callback);
extern "C" ERR ProcessMessages(PMF Flags, int TimeOut);
extern "C" ERR IdentifyFile(const std::string_view &Path, CLASSID Filter, CLASSID *Class, CLASSID *SubClass);
extern "C" ERR ReallocMemory(APTR Memory, uint32_t Size, APTR *Address);
extern "C" CLASSID ResolveClassName(const std::string_view &Name);
extern "C" ERR SendMessage(MSGID Type, MSF Flags, APTR Data, int Size);
extern "C" ERR SetOwner(OBJECTPTR Object, OBJECTPTR Owner);
extern "C" ERR ProtectMemory(APTR Address, MEM Flags);
extern "C" void SetObjectContext(OBJECTPTR Object, struct Field *Field, AC ActionID);
extern "C" CSTRING FieldName(uint32_t FieldID);
extern "C" ERR ScanDir(struct DirInfo *Info);
extern "C" ERR SetName(OBJECTPTR Object, const std::string_view &Name);
extern "C" void LogReturn(void);
extern "C" ERR SubscribeAction(OBJECTPTR Object, AC Action, FUNCTION *Callback);
extern "C" ERR SubscribeEvent(int64_t Event, FUNCTION *Callback, APTR *Handle);
extern "C" ERR SubscribeTimer(double Interval, FUNCTION *Callback, APTR *Subscription);
extern "C" ERR UpdateTimer(APTR Subscription, double Interval);
extern "C" ERR UnsubscribeAction(OBJECTPTR Object, AC Action);
extern "C" void UnsubscribeEvent(APTR Handle);
extern "C" ERR BroadcastEvent(APTR Event, int EventSize);
extern "C" ERR WaitTime(double Seconds);
extern "C" int64_t GetEventID(EVG Group, const std::string_view &SubGroup, const std::string_view &Event);
extern "C" uint32_t GenCRC32(uint32_t CRC, APTR Data, uint32_t Length);
extern "C" int64_t GetResource(RES Resource);
extern "C" int64_t SetResource(RES Resource, int64_t Value);
extern "C" ERR ScanMessages(int *Handle, MSGID Type, APTR Buffer, int Size);
extern "C" ERR WaitForObjects(PMF Flags, int TimeOut, struct ObjectSignal *ObjectSignals);
extern "C" void UnloadFile(struct CacheFile *Cache);
extern "C" ERR CreateFolder(const std::string_view &Path, PERMIT Permissions);
extern "C" ERR LoadFile(const std::string_view &Path, LDF Flags, struct CacheFile **Cache);
extern "C" ERR SetVolume(const std::string_view &Name, const std::string_view &Path, const std::string_view &Icon, const std::string_view &Label, const std::string_view &Device, VOLUME Flags);
extern "C" ERR DeleteVolume(const std::string_view &Name);
extern "C" ERR MoveFile(const std::string_view &Source, const std::string_view &Dest, FUNCTION *Callback);
extern "C" ERR UpdateMessage(int Message, MSGID Type, APTR Data, int Size);
extern "C" ERR AddMsgHandler(MSGID MsgType, FUNCTION *Routine, struct MsgHandler **Handle);
extern "C" ERR QueueAction(AC Action, OBJECTID Object, APTR Args);
extern "C" int64_t PreciseTime(void);
extern "C" ERR OpenDir(const std::string_view &Path, RDF Flags, struct DirInfo **Info);
extern "C" OBJECTPTR GetObjectPtr(OBJECTID Object);
extern "C" struct Field * FindField(OBJECTPTR Object, uint32_t FieldID, OBJECTPTR *Target);
extern "C" CSTRING GetErrorMsg(ERR Error);
extern "C" struct Message * GetActionMsg(AC Action);
extern "C" ERR FuncError(CSTRING Header, ERR Error);
extern "C" ERR LockObject(OBJECTPTR Object, int MilliSeconds);
extern "C" void ReleaseObject(OBJECTPTR Object);
extern "C" ERR AsyncAction(AC Action, OBJECTPTR Object, APTR Args, FUNCTION *Callback);
extern "C" ERR AddInfoTag(struct FileInfo *Info, const std::string_view &Name, const std::string_view &Value);
extern "C" void SetDefaultPermissions(int User, int Group, PERMIT Permissions);
extern "C" void VLogF(VLF Flags, const char *Header, const char *Message, va_list Args);
extern "C" ERR ReadInfoTag(struct FileInfo *Info, const std::string_view &Name, std::string_view *Value);
extern "C" ERR SetResourcePath(RP PathType, const std::string_view &Path);
extern "C" objTask * CurrentTask(void);
extern "C" CSTRING ResolveGroupID(int Group);
extern "C" CSTRING ResolveUserID(int User);
extern "C" ERR CreateLink(const std::string_view &From, const std::string_view &To);
extern "C" OBJECTPTR ParentContext(void);
extern "C" ERR WakeThread(int Thread, int Stop);
extern "C" ERR AsyncCancel(kt::vector<OBJECTID> &Objects);
extern "C" int AsyncPending(OBJECTID Object);
extern "C" ERR AsyncWait(kt::vector<OBJECTID> &Objects, int TimeOut);
extern "C" ERR ClassDatabase(kt::vector<ClassRecord *> *Classes);
#endif // KOTUKU_STATIC


//********************************************************************************************************************

template <pcObjectPointer T> inline MEMORYID GetMemoryID(T) {
   static_assert(not pcObjectPointer<T>, "GetMemoryID() cannot be called on object pointers; use Object->UID.");
   return 0;
}

template <class T> requires (not pcObjectPointer<T>) inline MEMORYID GetMemoryID(T &&A) {
   return ((MEMORYID *)A)[RESOURCE_ID_OFFSET];
}

inline ERR DeregisterFD(HOSTHANDLE Handle) {
   return RegisterFD(Handle, RFD::REMOVE|RFD::READ|RFD::WRITE|RFD::EXCEPT|RFD::ALWAYS_CALL, 0, 0);
}

inline APTR GetResourcePtr(RES ID) { return (APTR)(MAXINT)GetResource(ID); }

[[nodiscard]] constexpr inline const char* to_cstring(const char* A) { return A; }
[[nodiscard]] inline const char* to_cstring(const std::string &A) { return A.c_str(); }

#ifndef PRV_CORE_DATA
// These overloaded functions can't be used in the Core as they will confuse the compiler in key areas.

inline ERR SubscribeAction(OBJECTPTR Object, AC Action, FUNCTION Callback) {
   return SubscribeAction(Object,Action,&Callback);
}

inline ERR SubscribeEvent(int64_t Event, FUNCTION Callback, APTR *Handle) {
   return SubscribeEvent(Event,&Callback,Handle);
}

inline ERR SubscribeTimer(double Interval, FUNCTION Callback, APTR *Subscription) {
   return SubscribeTimer(Interval,&Callback,Subscription);
}

// This template leverages pcObject to be the preferred entry point for any Object type or derivation.

template <pcObject T> inline ERR FreeResource(T *Object) {
   if (not Object) return ERR::NullArgs;
   return FreeResource(Object->UID);
}

inline ERR FreeResource(const void *Address) {
   if (not Address) return ERR::NullArgs;
   return FreeResource(((const int *)Address)[RESOURCE_ID_OFFSET]);
}

template<class T> inline ERR NewObject(CLASSID ClassID, T **Result) {
   return NewObject(ClassID, NF::NIL, (OBJECTPTR *)Result);
}

template<class T> inline ERR NewLocalObject(CLASSID ClassID, T **Result) {
   return NewObject(ClassID, NF::LOCAL, (OBJECTPTR *)Result);
}

inline ERR MemoryInfo(MEMORYID ID, struct MemInfo * MemInfo) {
   return MemoryInfo(ID,MemInfo,sizeof(struct MemInfo));
}

inline ERR QueueAction(AC Action, OBJECTID ObjectID) {
   return QueueAction(Action, ObjectID, nullptr);
}
#endif

typedef KEYVALUE ConfigKeys;
typedef std::pair<std::string, ConfigKeys> ConfigGroup;
typedef std::vector<ConfigGroup> ConfigGroups;

namespace kt {

inline void copymem(const void *Src, APTR Dest, std::size_t Length) {
   memmove(Dest, Src, Length);
}

inline void clearmem(APTR Memory, std::size_t Length) {
   if (Memory) memset(Memory, 0, Length);
}

static thread_local int _tlUniqueThreadID = 0;

[[nodiscard]] inline int _get_thread_id(void) {
   if (_tlUniqueThreadID) return _tlUniqueThreadID;
   _tlUniqueThreadID = GetResource(RES::THREAD_ID);
   return _tlUniqueThreadID;
}

} // namespace

#include <kotuku/log.h>
#include <kotuku/objects.h>

inline OBJECTID CurrentTaskID() { return ((OBJECTPTR)CurrentTask())->UID; }
inline APTR SetResourcePtr(RES Res, APTR Value) { return (APTR)(MAXINT)(SetResource(Res, (MAXINT)Value)); }

// obj_read is used to build efficient customised jump tables for object calls.

struct obj_read {
   typedef int JUMP(struct lua_State *, const struct obj_read &, struct GCobject *);

   uint32_t Hash;
   JUMP *Call;
   APTR Data;

   auto operator<=>(const obj_read &Other) const {
       if (Hash < Other.Hash) return -1;
       if (Hash > Other.Hash) return 1;
       return 0;
   }

   obj_read(uint32_t pHash, JUMP pJump, APTR pData) : Hash(pHash), Call(pJump), Data(pData) { }
   obj_read(uint32_t pHash, JUMP pJump) : Hash(pHash), Call(pJump) { }
   obj_read(uint32_t pHash) : Hash(pHash) { }
};

inline auto read_hash = [](const obj_read &a, const obj_read &b) { return a.Hash < b.Hash; };

typedef std::vector<obj_read> READ_TABLE;

// Object field write handler structure for Tiri code

struct obj_write {
   typedef ERR JUMP(struct lua_State *, OBJECTPTR, struct Field *, int);

   uint32_t Hash;
   JUMP *Call;
   struct Field *Field;

   auto operator<=>(const obj_write &Other) const {
       if (Hash < Other.Hash) return -1;
       if (Hash > Other.Hash) return 1;
       return 0;
   }

   obj_write(uint32_t pHash, JUMP pJump, struct Field *pField) : Hash(pHash), Call(pJump), Field(pField) { }
   obj_write(uint32_t pHash, JUMP pJump) : Hash(pHash), Call(pJump) { }
   obj_write(uint32_t pHash) : Hash(pHash) { }
};

inline auto write_hash = [](const obj_write &a, const obj_write &b) { return a.Hash < b.Hash; };

typedef std::vector<obj_write> WRITE_TABLE;


// MetaClass class definition

#define VER_METACLASS (1.000000)

// MetaClass methods

namespace mc {
struct FindField { int ID; struct Field *Field; objMetaClass *Source; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objMetaClass : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::METACLASS;
   static constexpr CSTRING CLASS_NAME = "MetaClass";

   using create = kt::Create<objMetaClass>;

   double  ClassVersion;                // The version number of the class.
   const struct FieldArray * Fields;    // Points to a FieldArray that describes the class' object structure.
   struct Field * Dictionary;           // Returns a field lookup table sorted by field IDs.
   std::string ClassName;               // The name of the represented class.
   std::string FileExtension;           // Describes the file extension represented by the class.
   std::string FileDescription;         // Describes the file type represented by the class.
   std::string FileHeader;              // Defines a string expression that will allow relevant file data to be matched to the class.
   std::string Path;                    // The path to the module binary that represents the class.
   std::string Icon;                    // Associates an icon with the file data for this class.
   int     Size;                        // The total size of the object structure represented by the MetaClass.
   CLF     Flags;                       // Optional flag settings.
   CLASSID ClassID;                     // Specifies the ID of a class object.
   CLASSID BaseClassID;                 // Specifies the base class ID of a class object.
   int     OpenCount;                   // The total number of active objects that are linked back to the MetaClass.
   CCF     Category;                    // The system category that a class belongs to.

#ifdef PRV_METACLASS
    // Field table cache for Tiri - eliminates per-instance hash tables.
    READ_TABLE  ReadTable;
    WRITE_TABLE WriteTable;
#endif

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
   inline ERR findField(int ID, struct Field ** Field, objMetaClass ** Source) noexcept {
      struct mc::FindField args = { ID, (struct Field *)0, (objMetaClass *)0 };
      ERR error = Action(AC(-1), this, &args);
      if (Field) *Field = args.Field;
      if (Source) *Source = args.Source;
      return error;
   }

   // Customised field getting

   inline ERR getClassVersion(double &Value) noexcept {
      Value = this->ClassVersion;
      return ERR::Okay;
   }

   inline ERR getFields(const struct FieldArray * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[17];
      auto get_field = (ERR (*)(APTR, const struct FieldArray *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getDictionary(struct Field * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[6];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, struct Field *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      RestoreObjectContext();
      return error;
   }

   inline ERR getClassName(std::string_view &Value) noexcept {
      Value = this->ClassName;
      return ERR::Okay;
   }

   inline ERR getFileExtension(std::string_view &Value) noexcept {
      Value = this->FileExtension;
      return ERR::Okay;
   }

   inline ERR getFileDescription(std::string_view &Value) noexcept {
      Value = this->FileDescription;
      return ERR::Okay;
   }

   inline ERR getFileHeader(std::string_view &Value) noexcept {
      Value = this->FileHeader;
      return ERR::Okay;
   }

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getIcon(std::string_view &Value) noexcept {
      Value = this->Icon;
      return ERR::Okay;
   }

   inline ERR getSize(int &Value) noexcept {
      Value = this->Size;
      return ERR::Okay;
   }

   inline ERR getFlags(CLF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getClass(CLASSID &Value) noexcept {
      Value = this->ClassID;
      return ERR::Okay;
   }

   inline ERR getBaseClass(CLASSID &Value) noexcept {
      Value = this->BaseClassID;
      return ERR::Okay;
   }

   inline ERR getOpenCount(int &Value) noexcept {
      Value = this->OpenCount;
      return ERR::Okay;
   }

   inline ERR getCategory(CCF &Value) noexcept {
      Value = this->Category;
      return ERR::Okay;
   }

   inline ERR getMethods(APTR * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[19];
      auto get_field = (ERR (*)(APTR, APTR *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getActionTable(APTR * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto get_field = (ERR (*)(APTR, APTR *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getLocation(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getModule(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[24];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getObjects(int * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[25];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, int *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      RestoreObjectContext();
      return error;
   }

   inline ERR getSubClasses(OBJECTPTR * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[15];
      auto get_field = (ERR (*)(APTR, OBJECTPTR *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getRootModule(OBJECTPTR &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setClassVersion(const double Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->ClassVersion = Value;
      return ERR::Okay;
   }

   inline ERR setFields(const struct FieldArray * Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(this, field, 0x00101510, Value, Elements);
   }

   inline ERR setClassName(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->ClassName = Value;
      return ERR::Okay;
   }

   inline ERR setFileExtension(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->FileExtension = Value;
      return ERR::Okay;
   }

   inline ERR setFileDescription(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->FileDescription = Value;
      return ERR::Okay;
   }

   inline ERR setFileHeader(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->FileHeader = Value;
      return ERR::Okay;
   }

   inline ERR setPath(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Path = Value;
      return ERR::Okay;
   }

   inline ERR setIcon(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Icon = Value;
      return ERR::Okay;
   }

   inline ERR setSize(const int Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Size = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const CLF Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setClass(const CLASSID Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->ClassID = Value;
      return ERR::Okay;
   }

   inline ERR setBaseClass(const CLASSID Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->BaseClassID = Value;
      return ERR::Okay;
   }

   inline ERR setCategory(const CCF Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Category = Value;
      return ERR::Okay;
   }

   inline ERR setMethods(APTR Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(this, field, 0x00101510, Value, Elements);
   }

   inline ERR setActions(APTR Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, 0x08000400, Value, 1);
   }

   inline ERR setName(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(this, field, 0x00914500, &Value, 1);
   }

};

inline bool Object::isDerived() { return Class->ClassID != Class->BaseClassID; }
inline CLASSID Object::classID() { return Class->ClassID; }
inline CLASSID Object::baseClassID() { return Class->BaseClassID; }

// StorageDevice class definition

#define VER_STORAGEDEVICE (1.000000)

class objStorageDevice : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::STORAGEDEVICE;
   static constexpr CSTRING CLASS_NAME = "StorageDevice";

   using create = kt::Create<objStorageDevice>;

   DEVICE  DeviceFlags;   // These read-only flags identify the type of device and its features.
   int64_t DeviceSize;    // The storage size of the device in bytes, without accounting for the file system format.
   int64_t BytesFree;     // Total amount of storage space that is available, measured in bytes.
   int64_t BytesUsed;     // Total amount of storage space in use.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getDeviceFlags(DEVICE &Value) noexcept {
      Value = this->DeviceFlags;
      return ERR::Okay;
   }

   inline ERR getDeviceSize(int64_t &Value) noexcept {
      Value = this->DeviceSize;
      return ERR::Okay;
   }

   inline ERR getBytesFree(int64_t &Value) noexcept {
      Value = this->BytesFree;
      return ERR::Okay;
   }

   inline ERR getBytesUsed(int64_t &Value) noexcept {
      Value = this->BytesUsed;
      return ERR::Okay;
   }

   inline ERR getDevice(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getVolume(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }


   // Customised field setting

   inline ERR setVolume(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, 0x00904500, &Value, 1);
   }

};

// File class definition

#define VER_FILE (1.200000)

// File methods

namespace fl {
struct StartStream { OBJECTID SubscriberID; FL Flags; int Length; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct StopStream { static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Delete { FUNCTION *Callback; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Move { std::string_view Dest; FUNCTION *Callback; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Copy { std::string_view Dest; FUNCTION *Callback; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetDate { int Year; int Month; int Day; int Hour; int Minute; int Second; FDT Type; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ReadLine { std::string *Result; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct BufferContent { static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Next { objFile *File; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Watch { FUNCTION *Callback; MFF Flags; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objFile : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::FILE;
   static constexpr CSTRING CLASS_NAME = "File";

   using create = kt::Create<objFile>;

   int64_t  Position;   // The current read/write byte position in a file.
   std::string Path;    // Specifies the location of a file or folder.
   FL       Flags;      // File flags and options.
   int8_t * Buffer;     // Points to the internal data buffer if the file content is held in memory.
   public:
   inline std::string readLine() {
      std::string str;
      struct fl::ReadLine args { &str };
      Action(fl::ReadLine::id, this, &args);
      return str;
   }

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, const void *Buffer, int Size) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer, Size };
      return Action(AC::DataFeed, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR query() noexcept { return Action(AC::Query, this, nullptr); }
   template <class T, class U> ERR read(APTR Buffer, T Size, U *Result) noexcept {
      static_assert(std::is_integral<U>::value, "Result value must be an integer type");
      static_assert(std::is_integral<T>::value, "Size value must be an integer type");
      const int bytes = (Size > 0x7fffffff) ? 0x7fffffff : Size;
      struct acRead read = { (int8_t *)Buffer, bytes };
      if (auto error = Action(AC::Read, this, &read); error IS ERR::Okay) {
         *Result = static_cast<U>(read.Result);
         return ERR::Okay;
      }
      else { *Result = 0; return error; }
   }
   template <class T> ERR read(APTR Buffer, T Size) noexcept {
      static_assert(std::is_integral<T>::value, "Size value must be an integer type");
      const int bytes = (Size > 0x7fffffff) ? 0x7fffffff : Size;
      struct acRead read = { (int8_t *)Buffer, bytes };
      return Action(AC::Read, this, &read);
   }
   inline ERR rename(std::string_view Name) noexcept {
      struct acRename args = { Name };
      return Action(AC::Rename, this, &args);
   }
   inline ERR reset() noexcept { return Action(AC::Reset, this, nullptr); }
   inline ERR seek(double Offset, SEEK Position = SEEK::CURRENT) noexcept {
      struct acSeek args = { Offset, Position };
      return Action(AC::Seek, this, &args);
   }
   inline ERR seekStart(double Offset) noexcept { return seek(Offset, SEEK::START); }
   inline ERR seekEnd(double Offset) noexcept { return seek(Offset, SEEK::END); }
   inline ERR seekCurrent(double Offset) noexcept { return seek(Offset, SEEK::CURRENT); }
   inline ERR write(CPTR Buffer, int Size, int *Result = nullptr) noexcept {
      struct acWrite write = { (int8_t *)Buffer, Size };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline ERR write(std::string Buffer, int *Result = nullptr) noexcept {
      struct acWrite write = { (int8_t *)Buffer.c_str(), int(Buffer.size()) };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline ERR startStream(OBJECTID SubscriberID, FL Flags, int Length) noexcept {
      struct fl::StartStream args = { SubscriberID, Flags, Length };
      return Action(AC(-1), this, &args);
   }
   inline ERR stopStream() noexcept {
      return Action(AC(-2), this, nullptr);
   }
   inline ERR del(FUNCTION Callback) noexcept {
      struct fl::Delete args = { &Callback };
      return Action(AC(-3), this, &args);
   }
   inline ERR move(const std::string_view &Dest, FUNCTION Callback) noexcept {
      struct fl::Move args = { Dest, &Callback };
      return Action(AC(-4), this, &args);
   }
   inline ERR copy(const std::string_view &Dest, FUNCTION Callback) noexcept {
      struct fl::Copy args = { Dest, &Callback };
      return Action(AC(-5), this, &args);
   }
   inline ERR setDate(int Year, int Month, int Day, int Hour, int Minute, int Second, FDT Type) noexcept {
      struct fl::SetDate args = { Year, Month, Day, Hour, Minute, Second, Type };
      return Action(AC(-6), this, &args);
   }
   inline ERR readLine(std::string &Result) noexcept {
      struct fl::ReadLine args = { &Result };
      ERR error = Action(AC(-7), this, &args);
      return error;
   }
   inline ERR bufferContent() noexcept {
      return Action(AC(-8), this, nullptr);
   }
   inline ERR next(objFile ** File) noexcept {
      struct fl::Next args = { (objFile *)0 };
      ERR error = Action(AC(-9), this, &args);
      if (File) *File = args.File;
      return error;
   }
   inline ERR watch(FUNCTION Callback, MFF Flags) noexcept {
      struct fl::Watch args = { &Callback, Flags };
      return Action(AC(-10), this, &args);
   }

   // Customised field getting

   inline ERR getPosition(int64_t &Value) noexcept {
      Value = this->Position;
      return ERR::Okay;
   }

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getFlags(FL &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getBuffer(int8_t * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[12];
      auto get_field = (ERR (*)(APTR, int8_t *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getDate(APTR &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getCreated(APTR &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getHandle(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getIcon(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getPermissions(int &Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getResolvedPath(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[18];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getSize(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getTimestamp(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getLink(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getUser(int &Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getGroup(int &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setPosition(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, FD_INT64, &Value, 1);
   }

   inline ERR setPath(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, 0x00804500, &Value, 1);
   }

   inline ERR setFlags(const FL Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setDate(APTR Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, 0x08000310, Value, 1);
   }

   inline ERR setCreated(APTR Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, 0x08000310, Value, 1);
   }

   inline ERR setPermissions(const int Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setSize(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_INT64, &Value, 1);
   }

   inline ERR setLink(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(this, field, 0x00804300, &Value, 1);
   }

   inline ERR setUser(const int Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setGroup(const int Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// Config class definition

#define VER_CONFIG (1.000000)

// Config methods

namespace cfg {
struct ReadValue { std::string_view Group; std::string_view Key; std::string_view Data; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Set { std::string_view Group; std::string_view Key; std::string_view Data; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct WriteValue { std::string_view Group; std::string_view Key; std::string_view Data; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DeleteKey { std::string_view Group; std::string_view Key; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DeleteGroup { std::string_view Group; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetGroupFromIndex { int Index; std::string_view Group; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SortByKey { std::string_view Key; int Descending; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct MergeFile { std::string_view Path; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Merge { OBJECTPTR Source; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objConfig : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::CONFIG;
   static constexpr CSTRING CLASS_NAME = "Config";

   using create = kt::Create<objConfig>;

   std::string Path;         // Set this field to the location of the source configuration file.
   std::string KeyFilter;    // Set this field to enable key filtering.
   std::string GroupFilter;  // Set this field to enable group filtering.
   CNF Flags;                // Optional flags may be set here.
   public:
   ConfigGroups Groups;

   // For C++ only, these read variants avoid method calls for speed, but apply identical logic.

   inline ERR read(std::string_view pGroup, std::string_view pKey, double &pValue) {
      for (auto& [group, keys] : Groups) {
         if ((!pGroup.empty()) and (group.compare(pGroup))) continue;
         if (pKey.empty()) pValue = strtod(keys.cbegin()->second.c_str(), nullptr);
         else if (auto it = keys.find(pKey); it != keys.end()) pValue = strtod(it->second.c_str(), nullptr);
         else return ERR::Search;
         return ERR::Okay;
      }
      return ERR::Search;
   }

   inline ERR read(std::string_view pGroup, std::string_view pKey, int &pValue) {
      for (auto& [group, keys] : Groups) {
         if ((!pGroup.empty()) and (group.compare(pGroup))) continue;
         if (pKey.empty()) pValue = strtol(keys.cbegin()->second.c_str(), nullptr, 0);
         else if (auto it = keys.find(pKey); it != keys.end()) pValue = strtol(it->second.c_str(), nullptr, 0);
         else return ERR::Search;
         return ERR::Okay;
      }
      return ERR::Search;
   }

   inline ERR read(std::string_view pGroup, std::string_view pKey, std::string &pValue) {
      for (auto & [group, keys] : Groups) {
         if ((!pGroup.empty()) and (group.compare(pGroup))) continue;
         if (pKey.empty()) pValue = keys.cbegin()->second;
         else if (auto it = keys.find(pKey); it != keys.end()) pValue = it->second;
         else return ERR::Search;
         return ERR::Okay;
      }
      return ERR::Search;
   }

   inline ERR write(std::string_view Group, std::string_view Key, std::string_view Value) {
      for (auto& [group, keys] : Groups) {
         if (!group.compare(Group)) {
            if (auto it = keys.find(Key); it != keys.end()) {
               it->second.assign(Value);
            }
            else keys.emplace(Key, Value);
            return ERR::Okay;
         }
      }

      auto &new_group = Groups.emplace_back();
      new_group.first.assign(Group);
      new_group.second.emplace(Key, Value);
      return ERR::Okay;
   }

   // Action stubs

   inline ERR clear() noexcept { return Action(AC::Clear, this, nullptr); }
   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, const void *Buffer, int Size) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer, Size };
      return Action(AC::DataFeed, this, &args);
   }
   inline ERR flush() noexcept { return Action(AC::Flush, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR saveSettings() noexcept { return Action(AC::SaveSettings, this, nullptr); }
   inline ERR saveToObject(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveToObject args = { Dest, { ClassID } };
      return Action(AC::SaveToObject, this, &args);
   }
   inline ERR readValue(const std::string_view &Group, const std::string_view &Key, std::string_view * Data) noexcept {
      struct cfg::ReadValue args = { Group, Key };
      ERR error = Action(AC(-1), this, &args);
      if (Data) *Data = args.Data;
      return error;
   }
   inline ERR set(const std::string_view &Group, const std::string_view &Key, const std::string_view &Data) noexcept {
      struct cfg::Set args = { Group, Key, Data };
      return Action(AC(-2), this, &args);
   }
   inline ERR writeValue(const std::string_view &Group, const std::string_view &Key, const std::string_view &Data) noexcept {
      struct cfg::WriteValue args = { Group, Key, Data };
      return Action(AC(-3), this, &args);
   }
   inline ERR deleteKey(const std::string_view &Group, const std::string_view &Key) noexcept {
      struct cfg::DeleteKey args = { Group, Key };
      return Action(AC(-4), this, &args);
   }
   inline ERR deleteGroup(const std::string_view &Group) noexcept {
      struct cfg::DeleteGroup args = { Group };
      return Action(AC(-5), this, &args);
   }
   inline ERR getGroupFromIndex(int Index, std::string_view * Group) noexcept {
      struct cfg::GetGroupFromIndex args = { Index };
      ERR error = Action(AC(-6), this, &args);
      if (Group) *Group = args.Group;
      return error;
   }
   inline ERR sortByKey(const std::string_view &Key, int Descending) noexcept {
      struct cfg::SortByKey args = { Key, Descending };
      return Action(AC(-7), this, &args);
   }
   inline ERR mergeFile(const std::string_view &Path) noexcept {
      struct cfg::MergeFile args = { Path };
      return Action(AC(-9), this, &args);
   }
   inline ERR merge(OBJECTPTR Source) noexcept {
      struct cfg::Merge args = { Source };
      return Action(AC(-10), this, &args);
   }

   // Customised field getting

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getKeyFilter(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getGroupFilter(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getFlags(CNF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getData(APTR &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getTotalGroups(int &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getTotalKeys(int &Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setPath(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, 0x00804300, &Value, 1);
   }

   inline ERR setKeyFilter(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, 0x00904300, &Value, 1);
   }

   inline ERR setGroupFilter(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, 0x00904300, &Value, 1);
   }

   inline ERR setFlags(const CNF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

};

// Script class definition

#define VER_SCRIPT (1.000000)

// Script methods

namespace sc {
struct Exec { std::string_view Procedure; const struct ScriptArg *Args; int TotalArgs; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DerefProcedure { FUNCTION *Procedure; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Callback { int64_t ProcedureID; const struct ScriptArg *Args; int TotalArgs; ERR Error; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetProcedureID { std::string_view Procedure; int64_t ProcedureID; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DebugLog { std::string_view Options; std::string *Result; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objScript : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::SCRIPT;
   static constexpr CSTRING CLASS_NAME = "Script";

   using create = kt::Create<objScript>;

   std::string Path;            // The location of a script file to be loaded.
   std::string Procedure;       // Specifies a procedure name to be executed.
   std::string ErrorMessage;    // A human readable error string may be declared here following a script execution failure.
   OBJECTID TargetID;           // Reference to the default container that new script objects will be initialised to.
   SCF      Flags;              // Optional flags.
   ERR      Error;              // If a script fails during execution, an error code may be readable here.
   int      CurrentLine;        // Indicates the current line being executed when in debug mode.
   int      LineOffset;         // For debugging purposes, this value is added to any message referencing a line number.

#ifdef PRV_SCRIPT
   int64_t  ProcedureID;          // For callbacks
   KEYVALUE Vars;                 // Global parameters
   kt::vector<std::string> Results;
   const ScriptArg *ProcArgs;     // Procedure args - applies during Exec
   std::string String;
   std::string WorkingPath;
   std::string CacheFile;
   int      ActivationCount;      // Incremented every time the script is activated.
   int      TotalArgs;            // Total number of ProcArgs
   OBJECTID ScriptOwnerID;
#endif

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, const void *Buffer, int Size) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer, Size };
      return Action(AC::DataFeed, this, &args);
   }
   inline ERR getKey(std::string_view Key, std::string &Value) noexcept {
      struct acGetKey args = { Key, &Value };
      auto error = Action(AC::GetKey, this, &args);
      if (error != ERR::Okay) Value.clear();
      return error;
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR reset() noexcept { return Action(AC::Reset, this, nullptr); }
   inline ERR acSetKey(std::string_view FieldName, std::string_view Value) noexcept {
      struct acSetKey args = { FieldName, Value };
      return Action(AC::SetKey, this, &args);
   }
   inline ERR exec(const std::string_view &Procedure, const struct ScriptArg * Args, int TotalArgs) noexcept {
      struct sc::Exec args = { Procedure, Args, TotalArgs };
      return Action(AC(-1), this, &args);
   }
   inline ERR derefProcedure(FUNCTION Procedure) noexcept {
      struct sc::DerefProcedure args = { &Procedure };
      return Action(AC(-2), this, &args);
   }
   inline ERR callback(int64_t ProcedureID, const struct ScriptArg * Args, int TotalArgs, ERR * Error) noexcept {
      struct sc::Callback args = { ProcedureID, Args, TotalArgs, (ERR)0 };
      ERR error = Action(AC(-3), this, &args);
      if (Error) *Error = args.Error;
      return error;
   }
   inline ERR getProcedureID(const std::string_view &Procedure, int64_t * ProcedureID) noexcept {
      struct sc::GetProcedureID args = { Procedure, (int64_t)0 };
      ERR error = Action(AC(-4), this, &args);
      if (ProcedureID) *ProcedureID = args.ProcedureID;
      return error;
   }
   inline ERR debugLog(const std::string_view &Options, std::string &Result) noexcept {
      struct sc::DebugLog args = { Options, &Result };
      ERR error = Action(AC(-5), this, &args);
      return error;
   }

   // Customised field getting

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getProcedure(std::string_view &Value) noexcept {
      Value = this->Procedure;
      return ERR::Okay;
   }

   inline ERR getErrorMessage(std::string_view &Value) noexcept {
      Value = this->ErrorMessage;
      return ERR::Okay;
   }

   inline ERR getTarget(OBJECTID &Value) noexcept {
      Value = this->TargetID;
      return ERR::Okay;
   }

   inline ERR getFlags(SCF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getError(ERR &Value) noexcept {
      Value = this->Error;
      return ERR::Okay;
   }

   inline ERR getCurrentLine(int &Value) noexcept {
      Value = this->CurrentLine;
      return ERR::Okay;
   }

   inline ERR getLineOffset(int &Value) noexcept {
      Value = this->LineOffset;
      return ERR::Okay;
   }

   inline ERR getCacheFile(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[20];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getWorkingPath(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getResults(kt::vector<std::string> * &Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      auto get_field = (ERR (*)(APTR, kt::vector<std::string> *&))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getStatement(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getTotalArgs(int &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setPath(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, 0x00904500, &Value, 1);
   }

   inline ERR setProcedure(const std::string_view &Value) noexcept {
      this->Procedure = Value;
      return ERR::Okay;
   }

   inline ERR setErrorMessage(const std::string_view &Value) noexcept {
      this->ErrorMessage = Value;
      return ERR::Okay;
   }

   inline ERR setTarget(OBJECTID Value) noexcept {
      this->TargetID = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const SCF Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setLineOffset(const int Value) noexcept {
      this->LineOffset = Value;
      return ERR::Okay;
   }

   inline ERR setCacheFile(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[20];
      return field->WriteValue(this, field, 0x00904300, &Value, 1);
   }

   inline ERR setWorkingPath(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, 0x00804300, &Value, 1);
   }

   inline ERR setResults(const kt::vector<std::string> *Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(this, field, 0x00905300, Value, int(Value->size()));
   }

   inline ERR setStatement(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(this, field, 0x00904300, &Value, 1);
   }

};

namespace sc {
template <std::size_t SIZE> ERR Call(const FUNCTION &Function, const std::array<ScriptArg, SIZE> &Args) noexcept {
   struct Callback args = { Function.ProcedureID, Args.data(), int(std::ssize(Args)), ERR::Okay };
   return Action(sc::Callback::id, Function.Context, &args);
}

template <std::size_t SIZE> ERR Call(const FUNCTION &Function, const std::array<ScriptArg, SIZE> &Args, ERR &Result) noexcept {
   struct Callback args = { Function.ProcedureID, Args.data(), int(std::ssize(Args)), ERR::Okay };
   ERR error = Action(sc::Callback::id, Function.Context, &args);
   Result = args.Error;
   return(error);
}

inline ERR Call(const FUNCTION &Function) noexcept {
   struct Callback args = { Function.ProcedureID, nullptr, 0, ERR::Okay };
   return Action(sc::Callback::id, Function.Context, &args);
}

inline ERR Call(const FUNCTION &Function, ERR &Result) noexcept {
   struct Callback args = { Function.ProcedureID, nullptr, 0, ERR::Okay };
   ERR error = Action(sc::Callback::id, Function.Context, &args);
   Result = args.Error;
   return(error);
}
} // namespace
struct ActionEntry {
   ERR (*PerformAction)(OBJECTPTR, APTR);     // Pointer to a custom action hook.
};

// Task class definition

#define VER_TASK (1.000000)

// Task methods

namespace task {
struct Expunge { static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct AddArgument { std::string_view Argument; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Quit { static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetEnv { std::string_view Name; std::string *Value; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetEnv { std::string_view Name; std::string_view Value; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objTask : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::TASK;
   static constexpr CSTRING CLASS_NAME = "Task";

   using create = kt::Create<objTask>;

   std::string LaunchPath;    // Launched executables will start in the path specified here.
   std::string Name;          // Name of the task.
   std::string Location;      // Location of an executable file to launch.
   std::string Path;          // The current working folder of the active process.
   std::string ProcessPath;   // The path of the executable that is associated with the task.
   double TimeOut;            // Limits the amount of time to wait for a launched process to return.
   TSF    Flags;              // Optional flags.
   int    ReturnCode;         // The task's return code can be retrieved following execution.
   int    ProcessID;          // Reflects the process ID when an executable is launched.

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR getKey(std::string_view Key, std::string &Value) noexcept {
      struct acGetKey args = { Key, &Value };
      auto error = Action(AC::GetKey, this, &args);
      if (error != ERR::Okay) Value.clear();
      return error;
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR acSetKey(std::string_view FieldName, std::string_view Value) noexcept {
      struct acSetKey args = { FieldName, Value };
      return Action(AC::SetKey, this, &args);
   }
   inline ERR write(CPTR Buffer, int Size, int *Result = nullptr) noexcept {
      struct acWrite write = { (int8_t *)Buffer, Size };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline ERR write(std::string Buffer, int *Result = nullptr) noexcept {
      struct acWrite write = { (int8_t *)Buffer.c_str(), int(Buffer.size()) };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline ERR expunge() noexcept {
      return Action(AC(-1), this, nullptr);
   }
   inline ERR addArgument(const std::string_view &Argument) noexcept {
      struct task::AddArgument args = { Argument };
      return Action(AC(-2), this, &args);
   }
   inline ERR quit() noexcept {
      return Action(AC(-3), this, nullptr);
   }
   inline ERR getEnv(const std::string_view &Name, std::string &Value) noexcept {
      struct task::GetEnv args = { Name, &Value };
      ERR error = Action(AC(-4), this, &args);
      return error;
   }
   inline ERR setEnv(const std::string_view &Name, const std::string_view &Value) noexcept {
      struct task::SetEnv args = { Name, Value };
      return Action(AC(-5), this, &args);
   }

   // Customised field getting

   inline ERR getLaunchPath(std::string_view &Value) noexcept {
      Value = this->LaunchPath;
      return ERR::Okay;
   }

   inline ERR getName(std::string_view &Value) noexcept {
      Value = this->Name;
      return ERR::Okay;
   }

   inline ERR getLocation(std::string_view &Value) noexcept {
      Value = this->Location;
      return ERR::Okay;
   }

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getProcessPath(std::string_view &Value) noexcept {
      Value = this->ProcessPath;
      return ERR::Okay;
   }

   inline ERR getTimeOut(double &Value) noexcept {
      Value = this->TimeOut;
      return ERR::Okay;
   }

   inline ERR getFlags(TSF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getReturnCode(int &Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getProcess(int &Value) noexcept {
      Value = this->ProcessID;
      return ERR::Okay;
   }

   inline ERR getActions(APTR &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getAffinityMask(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getParameters(kt::vector<std::string> * &Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      auto get_field = (ERR (*)(APTR, kt::vector<std::string> *&))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getErrorCallback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[20];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getExitCallback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getInputCallback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getOutputCallback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getPriority(int &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setLaunchPath(const std::string_view &Value) noexcept {
      this->LaunchPath = Value;
      return ERR::Okay;
   }

   inline ERR setName(const std::string_view &Value) noexcept {
      this->Name = Value;
      return ERR::Okay;
   }

   inline ERR setLocation(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(this, field, 0x00804300, &Value, 1);
   }

   inline ERR setPath(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, 0x00804300, &Value, 1);
   }

   inline ERR setTimeOut(const double Value) noexcept {
      this->TimeOut = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const TSF Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setReturnCode(const int Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setProcess(const int Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->ProcessID = Value;
      return ERR::Okay;
   }

   inline ERR setAffinityMask(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_INT64, &Value, 1);
   }

   inline ERR setArgs(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(this, field, 0x00804200, &Value, 1);
   }

   inline ERR setParameters(const kt::vector<std::string> *Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      return field->WriteValue(this, field, 0x00905300, Value, int(Value->size()));
   }

   inline ERR setErrorCallback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[20];
      return field->WriteValue(this, field, FD_FUNCTION, &Value, 1);
   }

   inline ERR setExitCallback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_FUNCTION, &Value, 1);
   }

   inline ERR setInputCallback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(this, field, FD_FUNCTION, &Value, 1);
   }

   inline ERR setOutputCallback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_FUNCTION, &Value, 1);
   }

   inline ERR setPriority(const int Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// Thread class definition

#define VER_THREAD (1.000000)

// Thread methods

namespace th {
struct SetData { APTR Data; int Size; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objThread : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::THREAD;
   static constexpr CSTRING CLASS_NAME = "Thread";

   using create = kt::Create<objThread>;

   APTR Data;       // Pointer to initialisation data for the thread.
   int  DataSize;   // The size of the buffer referenced in the Data field.
   ERR  Error;      // Reflects the error code returned by the thread routine.
   THF  Flags;      // Optional flags can be defined here.

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR deactivate() noexcept { return Action(AC::Deactivate, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR setData(APTR Data, int Size) noexcept {
      struct th::SetData args = { Data, Size };
      return Action(AC(-1), this, &args);
   }

   // Customised field getting

   inline ERR getData(APTR * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[6];
      auto get_field = (ERR (*)(APTR, APTR *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getDataSize(int &Value) noexcept {
      Value = this->DataSize;
      return ERR::Okay;
   }

   inline ERR getError(ERR &Value) noexcept {
      Value = this->Error;
      return ERR::Okay;
   }

   inline ERR getFlags(THF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getCallback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getRoutine(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }


   // Customised field setting

   inline ERR setFlags(const THF Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setCallback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_FUNCTION, &Value, 1);
   }

   inline ERR setRoutine(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, FD_FUNCTION, &Value, 1);
   }

};

// Module class definition

#define VER_MODULE (1.000000)

// Module methods

namespace mod {
struct ResolveSymbol { std::string_view Name; APTR Address; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Test { std::string_view Options; int Passed; int Total; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objModule : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::MODULE;
   static constexpr CSTRING CLASS_NAME = "Module";

   using create = kt::Create<objModule>;

   const struct Function * FunctionList;    // Refers to a list of public functions exported by the module.
   APTR ModBase;                            // The Module's function base (jump table) must be read from this field.
   class RootModule * Root;                 // For internal use only.
   struct ModHeader * Header;               // For internal usage only.
   MOF  Flags;                              // Optional flags.
   public:
   static ERR load(std::string Name, OBJECTPTR *Module = nullptr, APTR Functions = nullptr) {
      if (auto module = objModule::create::global(kt::FieldValue(FID_Name, std::string_view(Name)))) {
         #ifdef KOTUKU_STATIC
            if (Module) *Module = module;
            if (Functions) ((APTR *)Functions)[0] = nullptr;
            return ERR::Okay;
         #else
            APTR functionbase;
            if (module->get(FID_ModBase, functionbase) IS ERR::Okay) {
               if (Module) *Module = module;
               if (Functions) ((APTR *)Functions)[0] = functionbase;
               return ERR::Okay;
            }
            else return ERR::GetField;
         #endif
      }
      else return ERR::CreateObject;
   }

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
   inline ERR resolveSymbol(const std::string_view &Name, APTR * Address) noexcept {
      struct mod::ResolveSymbol args = { Name, (APTR)0 };
      ERR error = Action(AC(-1), this, &args);
      if (Address) *Address = args.Address;
      return error;
   }
   inline ERR test(const std::string_view &Options, int * Passed, int * Total) noexcept {
      struct mod::Test args = { Options, (int)0, (int)0 };
      ERR error = Action(AC(-2), this, &args);
      if (Passed) *Passed = args.Passed;
      if (Total) *Total = args.Total;
      return error;
   }

   // Customised field getting

   inline ERR getFunctionList(const struct Function * &Value) noexcept {
      Value = this->FunctionList;
      return ERR::Okay;
   }

   inline ERR getModBase(APTR &Value) noexcept {
      Value = this->ModBase;
      return ERR::Okay;
   }

   inline ERR getFlags(MOF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getDefs(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getName(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }


   // Customised field setting

   inline ERR setFunctionList(const struct Function * Value) noexcept {
      this->FunctionList = Value;
      return ERR::Okay;
   }

   inline ERR setHeader(struct ModHeader * Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, 0x08000510, Value, 1);
   }

   inline ERR setFlags(const MOF Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setName(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, 0x00904500, &Value, 1);
   }

};

// Time class definition

#define VER_TIME (1.000000)

// Time methods

namespace pt {
struct SetTime { static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetTimeZoneInfo { std::string_view ZoneID; int StartYear; int EndYear; struct TimeZoneInfo *Info; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objTime : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::TIME;
   static constexpr CSTRING CLASS_NAME = "Time";

   using create = kt::Create<objTime>;

   int64_t SystemTime;    // Represents the system time when the time object was last queried.
   int     Year;          // Year (-ve for BC, +ve for AD).
   int     Month;         // Month (1 - 12)
   int     Day;           // Day (1 - 31)
   int     Hour;          // Hour (0 - 23)
   int     Minute;        // Minute (0 - 59)
   int     Second;        // Second (0 - 59)
   int     TimeZone;      // No information.
   int     DayOfWeek;     // Day of week (0 - 6) starting from Sunday.
   int     MilliSecond;   // A millisecond is one thousandth of a second (0 - 999)
   int     MicroSecond;   // A microsecond is one millionth of a second (0 - 999999)

   // Action stubs

   inline ERR query() noexcept { return Action(AC::Query, this, nullptr); }
   inline ERR refresh() noexcept { return Action(AC::Refresh, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR setTime() noexcept {
      return Action(AC(-1), this, nullptr);
   }
   inline ERR getTimeZoneInfo(const std::string_view &ZoneID, int StartYear, int EndYear, struct TimeZoneInfo ** Info) noexcept {
      struct pt::GetTimeZoneInfo args = { ZoneID, StartYear, EndYear, (struct TimeZoneInfo *)0 };
      ERR error = Action(AC(-2), this, &args);
      if (Info) *Info = args.Info;
      return error;
   }

   // Customised field getting

   inline ERR getSystemTime(int64_t &Value) noexcept {
      Value = this->SystemTime;
      return ERR::Okay;
   }

   inline ERR getYear(int &Value) noexcept {
      Value = this->Year;
      return ERR::Okay;
   }

   inline ERR getMonth(int &Value) noexcept {
      Value = this->Month;
      return ERR::Okay;
   }

   inline ERR getDay(int &Value) noexcept {
      Value = this->Day;
      return ERR::Okay;
   }

   inline ERR getHour(int &Value) noexcept {
      Value = this->Hour;
      return ERR::Okay;
   }

   inline ERR getMinute(int &Value) noexcept {
      Value = this->Minute;
      return ERR::Okay;
   }

   inline ERR getSecond(int &Value) noexcept {
      Value = this->Second;
      return ERR::Okay;
   }

   inline ERR getDayOfWeek(int &Value) noexcept {
      Value = this->DayOfWeek;
      return ERR::Okay;
   }

   inline ERR getMilliSecond(int &Value) noexcept {
      Value = this->MilliSecond;
      return ERR::Okay;
   }

   inline ERR getMicroSecond(int &Value) noexcept {
      Value = this->MicroSecond;
      return ERR::Okay;
   }

   inline ERR getTimestamp(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setSystemTime(const int64_t Value) noexcept {
      this->SystemTime = Value;
      return ERR::Okay;
   }

   inline ERR setYear(const int Value) noexcept {
      this->Year = Value;
      return ERR::Okay;
   }

   inline ERR setMonth(const int Value) noexcept {
      this->Month = Value;
      return ERR::Okay;
   }

   inline ERR setDay(const int Value) noexcept {
      this->Day = Value;
      return ERR::Okay;
   }

   inline ERR setHour(const int Value) noexcept {
      this->Hour = Value;
      return ERR::Okay;
   }

   inline ERR setMinute(const int Value) noexcept {
      this->Minute = Value;
      return ERR::Okay;
   }

   inline ERR setSecond(const int Value) noexcept {
      this->Second = Value;
      return ERR::Okay;
   }

   inline ERR setTimeZone(const int Value) noexcept {
      this->TimeZone = Value;
      return ERR::Okay;
   }

   inline ERR setDayOfWeek(const int Value) noexcept {
      this->DayOfWeek = Value;
      return ERR::Okay;
   }

   inline ERR setMilliSecond(const int Value) noexcept {
      this->MilliSecond = Value;
      return ERR::Okay;
   }

   inline ERR setMicroSecond(const int Value) noexcept {
      this->MicroSecond = Value;
      return ERR::Okay;
   }

};

// Compression class definition

#define VER_COMPRESSION (1.000000)

// Compression methods

namespace cmp {
struct CompressBuffer { APTR Input; int InputSize; APTR Output; int OutputSize; int Result; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CompressFile { std::string_view Location; std::string_view Path; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressBuffer { APTR Input; APTR Output; int OutputSize; int Result; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressFile { std::string_view Path; std::string_view Dest; int Flags; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RemoveFile { std::string_view Path; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CompressStream { APTR Input; int Length; FUNCTION *Callback; APTR Output; int OutputSize; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressStream { APTR Input; int Length; FUNCTION *Callback; APTR Output; int OutputSize; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CompressStreamStart { static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CompressStreamEnd { FUNCTION *Callback; APTR Output; int OutputSize; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressStreamEnd { FUNCTION *Callback; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressStreamStart { static const AC id = AC(-11); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressObject { std::string_view Path; OBJECTPTR Object; static const AC id = AC(-12); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Scan { std::string_view Folder; std::string_view Filter; FUNCTION *Callback; static const AC id = AC(-13); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Find { std::string_view Path; int CaseSensitive; int Wildcard; struct CompressedItem *Item; static const AC id = AC(-14); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objCompression : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::COMPRESSION;
   static constexpr CSTRING CLASS_NAME = "Compression";

   using create = kt::Create<objCompression>;

   int64_t  TotalOutput;   // The total number of bytes that have been output during the compression or decompression of streamed data.
   OBJECTID OutputID;      // Resulting messages will be sent to the object referred to in this field.
   int      CompressionLevel; // The compression level to use when compressing data.
   CMF      Flags;         // Optional flags.
   int      SegmentSize;   // Private. Splits the compressed file if it surpasses a set byte limit.
   PERMIT   Permissions;   // Default permissions for decompressed files are defined here.
   int      MinOutputSize; // Indicates the minimum output buffer size that will be needed during de/compression.
   int      WindowBits;    // Special option for certain compression formats.

   // Action stubs

   inline ERR flush() noexcept { return Action(AC::Flush, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR compressBuffer(APTR Input, int InputSize, APTR Output, int OutputSize, int * Result) noexcept {
      struct cmp::CompressBuffer args = { Input, InputSize, Output, OutputSize, (int)0 };
      ERR error = Action(AC(-1), this, &args);
      if (Result) *Result = args.Result;
      return error;
   }
   inline ERR compressFile(const std::string_view &Location, const std::string_view &Path) noexcept {
      struct cmp::CompressFile args = { Location, Path };
      return Action(AC(-2), this, &args);
   }
   inline ERR decompressBuffer(APTR Input, APTR Output, int OutputSize, int * Result) noexcept {
      struct cmp::DecompressBuffer args = { Input, Output, OutputSize, (int)0 };
      ERR error = Action(AC(-3), this, &args);
      if (Result) *Result = args.Result;
      return error;
   }
   inline ERR decompressFile(const std::string_view &Path, const std::string_view &Dest, int Flags) noexcept {
      struct cmp::DecompressFile args = { Path, Dest, Flags };
      return Action(AC(-4), this, &args);
   }
   inline ERR removeFile(const std::string_view &Path) noexcept {
      struct cmp::RemoveFile args = { Path };
      return Action(AC(-5), this, &args);
   }
   inline ERR compressStream(APTR Input, int Length, FUNCTION Callback, APTR Output, int OutputSize) noexcept {
      struct cmp::CompressStream args = { Input, Length, &Callback, Output, OutputSize };
      return Action(AC(-6), this, &args);
   }
   inline ERR decompressStream(APTR Input, int Length, FUNCTION Callback, APTR Output, int OutputSize) noexcept {
      struct cmp::DecompressStream args = { Input, Length, &Callback, Output, OutputSize };
      return Action(AC(-7), this, &args);
   }
   inline ERR compressStreamStart() noexcept {
      return Action(AC(-8), this, nullptr);
   }
   inline ERR compressStreamEnd(FUNCTION Callback, APTR Output, int OutputSize) noexcept {
      struct cmp::CompressStreamEnd args = { &Callback, Output, OutputSize };
      return Action(AC(-9), this, &args);
   }
   inline ERR decompressStreamEnd(FUNCTION Callback) noexcept {
      struct cmp::DecompressStreamEnd args = { &Callback };
      return Action(AC(-10), this, &args);
   }
   inline ERR decompressStreamStart() noexcept {
      return Action(AC(-11), this, nullptr);
   }
   inline ERR decompressObject(const std::string_view &Path, OBJECTPTR Object) noexcept {
      struct cmp::DecompressObject args = { Path, Object };
      return Action(AC(-12), this, &args);
   }
   inline ERR scan(const std::string_view &Folder, const std::string_view &Filter, FUNCTION Callback) noexcept {
      struct cmp::Scan args = { Folder, Filter, &Callback };
      return Action(AC(-13), this, &args);
   }
   inline ERR find(const std::string_view &Path, int CaseSensitive, int Wildcard, struct CompressedItem ** Item) noexcept {
      struct cmp::Find args = { Path, CaseSensitive, Wildcard, (struct CompressedItem *)0 };
      ERR error = Action(AC(-14), this, &args);
      if (Item) *Item = args.Item;
      return error;
   }

   // Customised field getting

   inline ERR getTotalOutput(int64_t &Value) noexcept {
      Value = this->TotalOutput;
      return ERR::Okay;
   }

   inline ERR getOutput(OBJECTID &Value) noexcept {
      Value = this->OutputID;
      return ERR::Okay;
   }

   inline ERR getCompressionLevel(int &Value) noexcept {
      Value = this->CompressionLevel;
      return ERR::Okay;
   }

   inline ERR getFlags(CMF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getPermissions(PERMIT &Value) noexcept {
      Value = this->Permissions;
      return ERR::Okay;
   }

   inline ERR getMinOutputSize(int &Value) noexcept {
      Value = this->MinOutputSize;
      return ERR::Okay;
   }

   inline ERR getWindowBits(int &Value) noexcept {
      Value = this->WindowBits;
      return ERR::Okay;
   }

   inline ERR getPath(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getFeedback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getPassword(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getSize(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getUncompressedSize(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setOutput(OBJECTID Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->OutputID = Value;
      return ERR::Okay;
   }

   inline ERR setCompressionLevel(const int Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setFlags(const CMF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setSegmentSize(const int Value) noexcept {
      this->SegmentSize = Value;
      return ERR::Okay;
   }

   inline ERR setPermissions(const PERMIT Value) noexcept {
      this->Permissions = Value;
      return ERR::Okay;
   }

   inline ERR setWindowBits(const int Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setArchiveName(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, 0x00804200, &Value, 1);
   }

   inline ERR setPath(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, 0x00804300, &Value, 1);
   }

   inline ERR setFeedback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(this, field, FD_FUNCTION, &Value, 1);
   }

   inline ERR setPassword(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(this, field, 0x00804300, &Value, 1);
   }

};

// CompressedStream class definition

#define VER_COMPRESSEDSTREAM (1.000000)

class objCompressedStream : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::COMPRESSEDSTREAM;
   static constexpr CSTRING CLASS_NAME = "CompressedStream";

   using create = kt::Create<objCompressedStream>;

   int64_t   TotalOutput;  // A live counter of total bytes that have been output by the stream.
   OBJECTPTR Input;        // An input object that will supply data for decompression.
   OBJECTPTR Output;       // A target object that will receive data compressed by the stream.
   CF        Format;       // The format of the compressed stream.  The default is GZIP.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getTotalOutput(int64_t &Value) noexcept {
      Value = this->TotalOutput;
      return ERR::Okay;
   }

   inline ERR getInput(OBJECTPTR &Value) noexcept {
      Value = this->Input;
      return ERR::Okay;
   }

   inline ERR getOutput(OBJECTPTR &Value) noexcept {
      Value = this->Output;
      return ERR::Okay;
   }

   inline ERR getFormat(CF &Value) noexcept {
      Value = this->Format;
      return ERR::Okay;
   }

   inline ERR getSize(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setInput(OBJECTPTR Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Input = Value;
      return ERR::Okay;
   }

   inline ERR setOutput(OBJECTPTR Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Output = Value;
      return ERR::Okay;
   }

   inline ERR setFormat(const CF Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Format = Value;
      return ERR::Okay;
   }

};

#ifdef __unix__
#include <pthread.h>
#endif

namespace kt {

#ifdef __system__
   struct ActionMessage {
      OBJECTID ObjectID;  // The object that is to receive the action
      int  Time;
      AC   ActionID;      // ID of the action or method to execute
      bool SendArgs;

      // Action arguments follow this structure in a buffer
   };
#endif

// Event support

struct Event {
   EVENTID EventID;
   // Data follows
};

#define EVID_DISPLAY_RESOLUTION_CHANGE  GetEventID(EVG::DISPLAY, "resolution", "change")

#define EVID_GUI_SURFACE_FOCUS          GetEventID(EVG::GUI, "surface", "focus")

#define EVID_FILESYSTEM_VOLUME_CREATED  GetEventID(EVG::FILESYSTEM, "volume", "created")
#define EVID_FILESYSTEM_VOLUME_DELETED  GetEventID(EVG::FILESYSTEM, "volume", "deleted")

#define EVID_SYSTEM_TASK_CREATED        GetEventID(EVG::SYSTEM, "task", "created")
#define EVID_SYSTEM_TASK_REMOVED        GetEventID(EVG::SYSTEM, "task", "removed")

#define EVID_POWER_STATE_SUSPENDING     GetEventID(EVG::POWER, "state", "suspending")
#define EVID_POWER_STATE_RESUMED        GetEventID(EVG::POWER, "state", "resumed")
#define EVID_POWER_DISPLAY_STANDBY      GetEventID(EVG::POWER, "display", "standby")
#define EVID_POWER_BATTERY_LOW          GetEventID(EVG::POWER, "battery", "low")
#define EVID_POWER_BATTERY_CRITICAL     GetEventID(EVG::POWER, "battery", "critical")
#define EVID_POWER_CPUTEMP_HIGH         GetEventID(EVG::POWER, "cputemp", "high")
#define EVID_POWER_CPUTEMP_CRITICAL     GetEventID(EVG::POWER, "cputemp", "critical")
#define EVID_POWER_SCREENSAVER_ON       GetEventID(EVG::POWER, "screensaver", "on")
#define EVID_POWER_SCREENSAVER_OFF      GetEventID(EVG::POWER, "screensaver", "off")

#define EVID_IO_KEYMAP_CHANGE           GetEventID(EVG::IO, "keymap", "change")
#define EVID_IO_KEYBOARD_KEYPRESS       GetEventID(EVG::IO, "keyboard", "keypress")

#define EVID_AUDIO_VOLUME_MASTER        GetEventID(EVG::AUDIO, "volume", "master")
#define EVID_AUDIO_VOLUME_LINEIN        GetEventID(EVG::AUDIO, "volume", "linein")
#define EVID_AUDIO_VOLUME_MIC           GetEventID(EVG::AUDIO, "volume", "mic")
#define EVID_AUDIO_VOLUME_MUTED         GetEventID(EVG::AUDIO, "volume", "muted") // All volumes have been muted
#define EVID_AUDIO_VOLUME_UNMUTED       GetEventID(EVG::AUDIO, "volume", "unmuted") // All volumes have been unmuted

// Event structures.

typedef struct { EVENTID EventID; char Name[1]; } evVolumeCreated;
typedef struct { EVENTID EventID; char Name[1]; } evVolumeDeleted;
typedef struct { EVENTID EventID; OBJECTID TaskID; } evTaskCreated;
typedef struct { EVENTID EventID; OBJECTID TaskID; OBJECTID ProcessID; } evTaskRemoved;
typedef struct { EVENTID EventID; } evPowerSuspending;
typedef struct { EVENTID EventID; } evPowerResumed;
typedef struct { EVENTID EventID; } evUserLogin;
typedef struct { EVENTID EventID; } evKeymapChange;
typedef struct { EVENTID EventID; } evScreensaverOn;
typedef struct { EVENTID EventID; } evScreensaverOff;
typedef struct { EVENTID EventID; double Volume; int Muted; } evVolume;
typedef struct { EVENTID EventID; KQ Qualifiers; KEY Code; int Unicode; } evKey;
typedef struct { EVENTID EventID; int16_t TotalWithFocus; int16_t TotalLostFocus; OBJECTID FocusList[1]; } evFocus;

// Hotplug event structure.  The hotplug event is sent whenever a new hardware device is inserted by the user.

struct evHotplug {
   EVENTID EventID;
   int16_t Type;        // HT ID
   int16_t Action;      // HTA_INSERTED, HTA_REMOVED
   int VendorID;        // USB vendor ID
   union {
      int ProductID;    // USB product or device ID
      int DeviceID;
   };
   char ID[20];         // Typically the PCI bus ID or USB bus ID, serial number or unique identifier
   char Group[32];      // Group name in the config file
   char Class[32];      // Class identifier (USB)
   char Product[40];    // Name of product or the hardware device
   char Vendor[40];     // Name of vendor
};

[[nodiscard]] inline char * strclone(const std::string_view String) noexcept
{
   char *newstr;
   if (AllocMemory(String.size()+1, MEM::STRING|MEM::NO_CLEAR, (APTR *)&newstr) IS ERR::Okay) {
      copymem(String.data(), newstr, String.size());
      newstr[String.size()] = 0;
      return newstr;
   }
   else return nullptr;
}

} // pf namespace

namespace fl {

// Read endian values from files and objects.

template<class T> ERR ReadLE(OBJECTPTR Object, T *Result)
{
   uint8_t data[sizeof(T)];
   struct acRead read = { .Buffer = data, .Length = sizeof(T) };
   if (Action(AC::Read, Object, &read) IS ERR::Okay) {
      if (read.Result IS sizeof(T)) {
         if constexpr (std::endian::native == std::endian::little) {
            *Result = ((T *)data)[0];
         }
         else {
            switch(sizeof(T)) {
               case 2:  *Result = (data[1]<<8) | data[0]; break;
               case 4:  *Result = (data[0]<<24)|(data[1]<<16)|(data[2]<<8)|(data[3]); break;
               case 8:  *Result = ((int64_t)data[0]<<56)|((int64_t)data[1]<<48)|((int64_t)data[2]<<40)|((int64_t)data[3]<<32)|(data[4]<<24)|(data[5]<<16)|(data[6]<<8)|(data[7]); break;
               default: *Result = ((T *)data)[0];
            }
         }
         return ERR::Okay;
      }
      else return ERR::Read;
   }
   else return ERR::Read;
}

template<class T> ERR ReadBE(OBJECTPTR Object, T *Result)
{
   uint8_t data[sizeof(T)];
   struct acRead read = { .Buffer = data, .Length = sizeof(T) };
   if (Action(AC::Read, Object, &read) IS ERR::Okay) {
      if (read.Result IS sizeof(T)) {
         if constexpr (std::endian::native == std::endian::little) {
            switch(sizeof(T)) {
               case 2:  *Result = (data[1]<<8) | data[0]; break;
               case 4:  *Result = (data[0]<<24)|(data[1]<<16)|(data[2]<<8)|(data[3]); break;
               case 8:  *Result = ((int64_t)data[0]<<56)|((int64_t)data[1]<<48)|((int64_t)data[2]<<40)|((int64_t)data[3]<<32)|(data[4]<<24)|(data[5]<<16)|(data[6]<<8)|(data[7]); break;
               default: *Result = ((T *)data)[0];
            }
         }
         else {
            *Result = ((T *)data)[0];
         }
         return ERR::Okay;
      }
      else return ERR::Read;
   }
   else return ERR::Read;
}

} // fl namespace

// Function construction (refer types.h)

template <class T, class X = APTR> FUNCTION C_FUNCTION(T *pRoutine, X pMeta = 0) {
   auto func    = FUNCTION(CALL::STD_C);
   func.Context = CurrentContext();
   func.Routine = (APTR)pRoutine;
   func.Meta    = reinterpret_cast<void *>(pMeta);
   return func;
};

inline CSTRING Object::className() { return Class->ClassName.c_str(); }
