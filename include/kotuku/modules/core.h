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
template <typename T> concept pcObject = std::is_base_of_v<Object, T>;

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

#define NETMSG_START 0
#define NETMSG_END 1

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
   END = 49,
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
   ALL_WRITE = 0x00000222,
   EVERYONE_WRITE = 0x00000222,
   EVERYONE_READWRITE = 0x00000333,
   OTHERS_EXEC = 0x00000400,
   ALL_EXEC = 0x00000444,
   EVERYONE_EXEC = 0x00000444,
   OTHERS_DELETE = 0x00000800,
   EVERYONE_DELETE = 0x00000888,
   ALL_DELETE = 0x00000888,
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
   MANAGED = 0x00000001,
   VIDEO = 0x00000002,
   TEXTURE = 0x00000004,
   AUDIO = 0x00000008,
   CODE = 0x00000010,
   NO_POOL = 0x00000020,
   TMP_LOCK = 0x00000040,
   UNTRACKED = 0x00000080,
   STRING = 0x00000100,
   OBJECT = 0x00000200,
   NO_LOCK = 0x00000400,
   EXCLUSIVE = 0x00000800,
   COLLECT = 0x00001000,
   NO_BLOCKING = 0x00002000,
   NO_BLOCK = 0x00002000,
   PROTECTED = 0x00004000,
   READ = 0x00010000,
   WRITE = 0x00020000,
   READ_WRITE = 0x00030000,
   NO_CLEAR = 0x00040000,
   HIDDEN = 0x00100000,
   CALLER = 0x00800000,
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

// Gamepad controller buttons.

enum class CON : uint32_t {
   NIL = 0,
   GAMEPAD_S = 0x00000001,
   GAMEPAD_E = 0x00000002,
   GAMEPAD_W = 0x00000004,
   GAMEPAD_N = 0x00000008,
   DPAD_UP = 0x00000010,
   DPAD_DOWN = 0x00000020,
   DPAD_LEFT = 0x00000040,
   DPAD_RIGHT = 0x00000080,
   START = 0x00000100,
   SELECT = 0x00000200,
   LEFT_BUMPER_1 = 0x00000400,
   LEFT_BUMPER_2 = 0x00000800,
   RIGHT_BUMPER_1 = 0x00001000,
   RIGHT_BUMPER_2 = 0x00002000,
   LEFT_THUMB = 0x00004000,
   RIGHT_THUMB = 0x00008000,
};

DEFINE_ENUM_FLAG_OPERATORS(CON)

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
#define FD_VOLATILE 0x00000000
#define FD_VOID 0x00000000
#define FD_OBJECT 0x00000001
#define FD_LOCAL 0x00000002
#define FD_VIRTUAL 0x00000008
#define FD_STRUCT 0x00000010
#define FD_ALLOC 0x00000020
#define FD_FLAGS 0x00000040
#define FD_VARTAGS 0x00000040
#define FD_LOOKUP 0x00000080
#define FD_PTRSIZE 0x00000080
#define FD_BUFSIZE 0x00000080
#define FD_ARRAYSIZE 0x00000080
#define FD_R 0x00000100
#define FD_READ 0x00000100
#define FD_RESULT 0x00000100
#define FD_BUFFER 0x00000200
#define FD_W 0x00000200
#define FD_WRITE 0x00000200
#define FD_RW 0x00000300
#define FD_INIT 0x00000400
#define FD_I 0x00000400
#define FD_TAGS 0x00000400
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
#define FD_RGB 0x00080000
#define FD_SCALED 0x00200000
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
   LOG_ALL = 0x00000002,
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
   ERROR = 0x00000080,
   PRIVILEGED = 0x00000100,
   SYSTEM_PATH = 0x00000200,
   MODULE_PATH = 0x00000400,
   ROOT_PATH = 0x00000800,
   SCAN_MODULES = 0x00001000,
};

DEFINE_ENUM_FLAG_OPERATORS(OPF)

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
   HIDDEN = 0x00000100,
   QUALIFY = 0x00000200,
   QUALIFIED = 0x00000200,
   VIRTUAL = 0x00000400,
   STREAM = 0x00000800,
   READ_ONLY = 0x00001000,
   ARCHIVE = 0x00002000,
   OPENDIR = 0x00004000,
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

// Flags that can be passed to FindObject()

enum class FOF : uint32_t {
   NIL = 0,
   SMART_NAMES = 0x00000001,
};

DEFINE_ENUM_FLAG_OPERATORS(FOF)

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
   COLLECT = 0x00000080,
   RECLASSED = 0x00000100,
   MESSAGE = 0x00000200,
   SIGNALLED = 0x00000400,
   PERMIT_TERMINATE = 0x00000800,
   ASYNC_ACTIVE = 0x00001000,
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
   CORE_IDL = 8,
   STATIC_BUILD = 9,
   LOG_LEVEL = 10,
   TOTAL_SHARED_MEMORY = 11,
   LOG_DEPTH = 12,
   JNI_ENV = 13,
   THREAD_ID = 14,
   OPEN_INFO = 15,
   EXCEPTION_HANDLER = 16,
   NET_PROCESSING = 17,
   PROCESS_STATE = 18,
   TOTAL_MEMORY = 19,
   TOTAL_SWAP = 20,
   CPU_SPEED = 21,
   FREE_MEMORY = 22,
   MEMORY_USAGE = 23,
   MAIN_THREAD = 24,
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

#define AHASH_ACTIVATE 0xdbaf4876
#define AHASH_CLEAR 0x0f3b6d8c
#define AHASH_FREEWARNING 0xb903ddbd
#define AHASH_COPYDATA 0x47b0d1fa
#define AHASH_DATAFEED 0x05e6d293
#define AHASH_DEACTIVATE 0x1ee323ff
#define AHASH_DRAW 0x7c95d753
#define AHASH_FLUSH 0x0f71fd67
#define AHASH_FOCUS 0x0f735645
#define AHASH_FREE 0x7c96f087
#define AHASH_GETKEY 0xff87790e
#define AHASH_DRAGDROP 0xf69e8a58
#define AHASH_HIDE 0x7c97e2df
#define AHASH_INIT 0x7c988539
#define AHASH_LOCK 0x7c9a2dce
#define AHASH_LOSTFOCUS 0x319b8e67
#define AHASH_MOVE 0x7c9abc9c
#define AHASH_MOVETOBACK 0xcbdb3170
#define AHASH_MOVETOFRONT 0x479347c8
#define AHASH_NEWCHILD 0x7b86ebf3
#define AHASH_NEWOWNER 0x7c68601a
#define AHASH_NEWOBJECT 0x07f62dc6
#define AHASH_REDO 0x7c9d4daf
#define AHASH_QUERY 0x103db63b
#define AHASH_READ 0x7c9d4d41
#define AHASH_RENAME 0x192cc41d
#define AHASH_RESET 0x10474288
#define AHASH_RESIZE 0x192fa5b7
#define AHASH_SAVEIMAGE 0x398f7c57
#define AHASH_SAVETOOBJECT 0x2878872e
#define AHASH_SEEK 0x7c9dda2d
#define AHASH_SETKEY 0x1b85609a
#define AHASH_SHOW 0x7c9de846
#define AHASH_TIMER 0x106d8b86
#define AHASH_UNLOCK 0x20ce3c11
#define AHASH_NEXT 0x7c9b1ec4
#define AHASH_PREV 0x7c9c6c62
#define AHASH_WRITE 0x10a8b550
#define AHASH_SETFIELD 0x12075f55
#define AHASH_CLIPBOARD 0x4912a9b5
#define AHASH_REFRESH 0x3e3db654
#define AHASH_DISABLE 0x12c4e4b9
#define AHASH_ENABLE 0xfb7573ac
#define AHASH_REDIMENSION 0x08a67fa2
#define AHASH_MOVETOPOINT 0x48467e29
#define AHASH_SORT 0x7c9e066d
#define AHASH_SAVESETTINGS 0x475f7165
#define AHASH_SIGNAL 0x1bc6ade3
#define AHASH_NEWPLACEMENT 0x9b0a0468
#define AHASH_UNDO 0x7c9f191b


typedef AC ACTIONID;

#ifndef __GNUC__
#define __attribute__(a)
#endif

typedef const std::vector<std::pair<std::string, uint32_t>> STRUCTS;
typedef std::map<std::string, std::string, std::less<>> KEYVALUE;

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

namespace pf {

template <class T> T roundup(T Num, int Alignment) {
   return (Num + Alignment) - (Num % Alignment); // Round up to Alignment value, e.g. (14,8) = 16
}

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

// Fast float-2-int conversion, with rounding to the nearest integer (F2I)

#if defined(__GNUC__) && defined(__x86__)

constexpr int F2I(double val) noexcept {
   if (std::is_constant_evaluated()) return (int)std::round(val);
   // This will round if the CPU is kept in its default rounding mode
   int ret;
   asm ("fistpl %0" : "=m" (ret) : "t" (val) : "st");
   return(ret);
}

#else

inline int F2I(double val) {
   double t = val + 6755399441055744.0;
   return *((int *)(&t));
}

#endif

} // namespace

// Structures to pass to OpenCore()

struct OpenTag {
   TOI Tag;
   union {
      int Int;
      int64_t Int64;
      APTR Pointer;
      CSTRING String;
   } Value;
};

struct OpenInfo {
   CSTRING Name;            // OPF::NAME
   CSTRING *Args;           // OPF::ARGS
   CSTRING SystemPath;      // OPF::SYSTEM_PATH
   CSTRING ModulePath;      // OPF::MODULE_PATH
   CSTRING RootPath;        // OPF::ROOT_PATH
   OpenTag *Options;        // OPF::OPTIONS Typecast to va_list (defined in stdarg.h)
   OPF     Flags;           // OPF::flags need to be set for fields that have been defined in this structure.
   int     MaxDepth;        // OPF::MAX_DEPTH
   int     Detail;          // OPF::DETAIL
   int     ArgCount;        // OPF::ARGS
   ERR     Error;           // OPF::ERROR
};

#ifdef _LP64
#define FD_PTR64 FD_POINTER
#else
#define FD_PTR64 0
#endif

#define nextutf8(str) if (*(str)) for (++(str); (*(str) & 0xc0) IS 0x80; (str)++);

//********************************************************************************************************************

namespace pf {

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


struct ObjectSignal {
   OBJECTPTR Object;    // Reference to an object to monitor.
};

struct ResourceManager {
   CSTRING Name;          // The name of the resource.
   ERR (*Free)(APTR);     // A function that will remove the resource's content when terminated.
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
using ModTest    = void (*)(CSTRING, int *, int *);
struct ModHeader {
   MHF     Flags;                                 // Special flags, type of function table wanted from the Core
   CSTRING Definitions;                           // Module definition string, usable by run-time languages such as Tiri
   ERR (*Init)(OBJECTPTR, struct CoreBase *);     // A one-off initialisation routine for when the module is first opened.
   void (*Close)(OBJECTPTR);                      // A function that will be called each time the module is closed.
   ERR (*Open)(OBJECTPTR);                        // A function that will be called each time the module is opened.
   ERR (*Expunge)(void);                          // Reference to an expunge function to terminate the module.
   void (*Test)(CSTRING, int *, int *);           // A function that can run embedded unit tests in development builds.
   CSTRING Name;                                  // Name of the module
   CSTRING Namespace;                             // A reserved system-wide namespace for function names.
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
   MAXINT   Arg;    // Can be a pointer or an integer value
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

struct SystemState {
   CSTRING Platform;        // String-based field indicating the user's platform.  Currently returns Native, Windows, OSX or Linux.
   HOSTHANDLE ConsoleFD;    // Internal
   int     Stage;           // The current operating stage.  -1 = Initialising, 0 indicates normal operating status; 1 means that the program is shutting down; 2 indicates a program restart; 3 is for mode switches.
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
   OBJECTID ObjectID;    // The object that owns the memory block.
   uint32_t Size;        // The size of the memory block.
   MEM      Flags;       // The type of memory.
   MEMORYID MemoryID;    // The unique ID for this block.
   int16_t  AccessCount; // Total number of active locks on this block.
} MEMINFO;

struct MsgHandler {
   struct MsgHandler * Prev;    // Previous message handler in the chain.
   struct MsgHandler * Next;    // Next message handler in the chain.
   FUNCTION Function;           // Call this function to handle the message.
   MSGID    MsgType;            // Type of message being filtered.
};

struct CacheFile {
   int64_t TimeStamp;    // The file's last-modified timestamp.
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
   int64_t TimeStamp;         // 64-bit time stamp - usable only for comparison (e.g. sorting).
   struct FileInfo * Next;    // Next structure in the list, or NULL.
   STRING  Name;              // The name of the file.
   RDF     Flags;             // Additional flags to describe the file.
   PERMIT  Permissions;       // Standard permission flags.
   int     UserID;            // User  ID (Unix systems only).
   int     GroupID;           // Group ID (Unix systems only).
   struct DateTime Created;   // The date/time of the file's creation.
   struct DateTime Modified;  // The date/time of the last file modification.
    ankerl::unordered_dense::map<std::string, std::string> *Tags;
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
   ERR (*GetValue)(APTR, APTR);                                              // A virtual function that will retrieve the value for this field.
   APTR     SetValue;                                                        // A virtual function that will set the value for this field.
   ERR (*WriteValue)(OBJECTPTR, struct Field *, int, const void *, int);     // An internal function for writing to this field.
   CSTRING  Name;                                                            // The English name for the field, e.g. Width
   uint32_t FieldID;                                                         // 32-bit hash from fieldhash(). Represented by FID constants, e.g. FID_Width
   uint16_t Offset;                                                          // Field offset within the object
   uint16_t Index;                                                           // Field array index
   uint32_t Flags;                                                           // Special flags that describe the field
   inline bool readable() { return (Flags & FD_READ) ? true : false; }
   inline bool writeable() { return (Flags & (FD_WRITE|FD_INIT)) ? true : false; }
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
   ERR (*_AccessMemory)(MEMORYID Memory, MEM Flags, int MilliSeconds, APTR *Result);
   ERR (*_Action)(AC Action, OBJECTPTR Object, APTR Parameters);
   void (*_ActionList)(struct ActionTable **Actions, int *Size);
   ERR (*_DeleteFile)(CSTRING Path, FUNCTION *Callback);
   CSTRING (*_ResolveClassID)(CLASSID ID);
   int (*_AllocateID)(IDTYPE Type);
   ERR (*_AllocMemory)(int Size, MEM Flags, APTR *Address, MEMORYID *ID);
   ERR (*_AccessObject)(OBJECTID Object, int MilliSeconds, OBJECTPTR *Result);
   ERR (*_CheckAction)(OBJECTPTR Object, AC Action);
   ERR (*_CheckMemoryExists)(MEMORYID ID);
   ERR (*_CheckObjectExists)(OBJECTID Object);
   ERR (*_InitObject)(OBJECTPTR Object);
   ERR (*_VirtualVolume)(CSTRING Name, ...);
   OBJECTPTR (*_CurrentContext)(void);
   int (*_AdjustLogLevel)(int Delta);
   ERR (*_ReadFileToBuffer)(CSTRING Path, APTR Buffer, int BufferSize, int *Result);
   ERR (*_FindObject)(CSTRING Name, CLASSID ClassID, FOF Flags, OBJECTID *ObjectID);
   objMetaClass * (*_FindClass)(CLASSID ClassID);
   ERR (*_AnalysePath)(CSTRING Path, LOC *Type);
   ERR (*_FreeResource)(MEMORYID ID);
   CLASSID (*_GetClassID)(OBJECTID Object);
   OBJECTID (*_GetOwnerID)(OBJECTID Object);
   ERR (*_CompareFilePaths)(CSTRING PathA, CSTRING PathB);
   const struct SystemState * (*_GetSystemState)(void);
   ERR (*_ListChildren)(OBJECTID Object, pf::vector<ChildEntry> *List);
   ERR (*_RegisterFD)(HOSTHANDLE FD, RFD Flags, void (*Routine)(HOSTHANDLE, APTR) , APTR Data);
   ERR (*_ResolvePath)(const std::string_view & Path, RSF Flags, std::string *Result);
   ERR (*_MemoryIDInfo)(MEMORYID ID, struct MemInfo *MemInfo, int Size);
   ERR (*_MemoryPtrInfo)(APTR Address, struct MemInfo *MemInfo, int Size);
   ERR (*_NewObject)(CLASSID ClassID, NF Flags, OBJECTPTR *Object);
   void (*_NotifySubscribers)(OBJECTPTR Object, AC Action, APTR Args, ERR Error);
   ERR (*_CopyFile)(CSTRING Source, CSTRING Dest, FUNCTION *Callback);
   ERR (*_ProcessMessages)(PMF Flags, int TimeOut);
   ERR (*_IdentifyFile)(CSTRING Path, CLASSID Filter, CLASSID *Class, CLASSID *SubClass);
   ERR (*_ReallocMemory)(APTR Memory, uint32_t Size, APTR *Address, MEMORYID *ID);
   ERR (*_ReleaseMemory)(MEMORYID MemoryID);
   CLASSID (*_ResolveClassName)(CSTRING Name);
   ERR (*_SendMessage)(MSGID Type, MSF Flags, APTR Data, int Size);
   ERR (*_SetOwner)(OBJECTPTR Object, OBJECTPTR Owner);
   ERR (*_ProtectMemory)(APTR Address, MEM Flags);
   void (*_SetObjectContext)(OBJECTPTR Object, struct Field *Field, AC ActionID);
   CSTRING (*_FieldName)(uint32_t FieldID);
   ERR (*_ScanDir)(struct DirInfo *Info);
   ERR (*_SetName)(OBJECTPTR Object, CSTRING Name);
   void (*_LogReturn)(void);
   ERR (*_SubscribeAction)(OBJECTPTR Object, AC Action, FUNCTION *Callback);
   ERR (*_SubscribeEvent)(int64_t Event, FUNCTION *Callback, APTR *Handle);
   ERR (*_SubscribeTimer)(double Interval, FUNCTION *Callback, APTR *Subscription);
   ERR (*_UpdateTimer)(APTR Subscription, double Interval);
   ERR (*_UnsubscribeAction)(OBJECTPTR Object, AC Action);
   void (*_UnsubscribeEvent)(APTR Handle);
   ERR (*_BroadcastEvent)(APTR Event, int EventSize);
   ERR (*_WaitTime)(double Seconds);
   int64_t (*_GetEventID)(EVG Group, CSTRING SubGroup, CSTRING Event);
   uint32_t (*_GenCRC32)(uint32_t CRC, APTR Data, uint32_t Length);
   int64_t (*_GetResource)(RES Resource);
   int64_t (*_SetResource)(RES Resource, int64_t Value);
   ERR (*_ScanMessages)(int *Handle, MSGID Type, APTR Buffer, int Size);
   ERR (*_WaitForObjects)(PMF Flags, int TimeOut, struct ObjectSignal *ObjectSignals);
   void (*_UnloadFile)(struct CacheFile *Cache);
   ERR (*_CreateFolder)(CSTRING Path, PERMIT Permissions);
   ERR (*_LoadFile)(CSTRING Path, LDF Flags, struct CacheFile **Cache);
   ERR (*_SetVolume)(CSTRING Name, CSTRING Path, CSTRING Icon, CSTRING Label, CSTRING Device, VOLUME Flags);
   ERR (*_DeleteVolume)(CSTRING Name);
   ERR (*_MoveFile)(CSTRING Source, CSTRING Dest, FUNCTION *Callback);
   ERR (*_UpdateMessage)(int Message, MSGID Type, APTR Data, int Size);
   ERR (*_AddMsgHandler)(MSGID MsgType, FUNCTION *Routine, struct MsgHandler **Handle);
   ERR (*_QueueAction)(AC Action, OBJECTID Object, APTR Args);
   int64_t (*_PreciseTime)(void);
   ERR (*_OpenDir)(CSTRING Path, RDF Flags, struct DirInfo **Info);
   OBJECTPTR (*_GetObjectPtr)(OBJECTID Object);
   struct Field * (*_FindField)(OBJECTPTR Object, uint32_t FieldID, OBJECTPTR *Target);
   CSTRING (*_GetErrorMsg)(ERR Error);
   struct Message * (*_GetActionMsg)(void);
   ERR (*_FuncError)(CSTRING Header, ERR Error);
   ERR (*_LockObject)(OBJECTPTR Object, int MilliSeconds);
   void (*_ReleaseObject)(OBJECTPTR Object);
   ERR (*_AsyncAction)(AC Action, OBJECTPTR Object, APTR Args, FUNCTION *Callback);
   ERR (*_AddInfoTag)(struct FileInfo *Info, CSTRING Name, CSTRING Value);
   void (*_SetDefaultPermissions)(int User, int Group, PERMIT Permissions);
   void (*_VLogF)(VLF Flags, const char *Header, const char *Message, va_list Args);
   ERR (*_ReadInfoTag)(struct FileInfo *Info, CSTRING Name, CSTRING *Value);
   ERR (*_SetResourcePath)(RP PathType, CSTRING Path);
   objTask * (*_CurrentTask)(void);
   CSTRING (*_ResolveGroupID)(int Group);
   CSTRING (*_ResolveUserID)(int User);
   ERR (*_CreateLink)(CSTRING From, CSTRING To);
   OBJECTPTR (*_ParentContext)(void);
   void (*_SetResourceMgr)(APTR Address, struct ResourceManager *Manager);
   ERR (*_WakeThread)(int Thread, int Stop);
   ERR (*_AsyncCancel)(OBJECTID *Objects, int Size);
   int (*_AsyncPending)(OBJECTID Object);
   ERR (*_AsyncWait)(OBJECTID *Objects, int Size, int TimeOut);
#endif // KOTUKU_STATIC
};

#if !defined(KOTUKU_STATIC) and !defined(PRV_CORE_MODULE)
extern struct CoreBase *CoreBase;
inline ERR AccessMemory(MEMORYID Memory, MEM Flags, int MilliSeconds, APTR *Result) { return CoreBase->_AccessMemory(Memory,Flags,MilliSeconds,Result); }
inline ERR Action(AC Action, OBJECTPTR Object, APTR Parameters) { return CoreBase->_Action(Action,Object,Parameters); }
inline void ActionList(struct ActionTable **Actions, int *Size) { return CoreBase->_ActionList(Actions,Size); }
inline ERR DeleteFile(CSTRING Path, FUNCTION *Callback) { return CoreBase->_DeleteFile(Path,Callback); }
inline CSTRING ResolveClassID(CLASSID ID) { return CoreBase->_ResolveClassID(ID); }
inline int AllocateID(IDTYPE Type) { return CoreBase->_AllocateID(Type); }
inline ERR AllocMemory(int Size, MEM Flags, APTR *Address, MEMORYID *ID) { return CoreBase->_AllocMemory(Size,Flags,Address,ID); }
inline ERR AccessObject(OBJECTID Object, int MilliSeconds, OBJECTPTR *Result) { return CoreBase->_AccessObject(Object,MilliSeconds,Result); }
inline ERR CheckAction(OBJECTPTR Object, AC Action) { return CoreBase->_CheckAction(Object,Action); }
inline ERR CheckMemoryExists(MEMORYID ID) { return CoreBase->_CheckMemoryExists(ID); }
inline ERR CheckObjectExists(OBJECTID Object) { return CoreBase->_CheckObjectExists(Object); }
inline ERR InitObject(OBJECTPTR Object) { return CoreBase->_InitObject(Object); }
template<class... Args> ERR VirtualVolume(CSTRING Name, Args... Tags) { return CoreBase->_VirtualVolume(Name,Tags...); }
inline OBJECTPTR CurrentContext(void) { return CoreBase->_CurrentContext(); }
inline int AdjustLogLevel(int Delta) { return CoreBase->_AdjustLogLevel(Delta); }
inline ERR ReadFileToBuffer(CSTRING Path, APTR Buffer, int BufferSize, int *Result) { return CoreBase->_ReadFileToBuffer(Path,Buffer,BufferSize,Result); }
inline ERR FindObject(CSTRING Name, CLASSID ClassID, FOF Flags, OBJECTID *ObjectID) { return CoreBase->_FindObject(Name,ClassID,Flags,ObjectID); }
inline objMetaClass * FindClass(CLASSID ClassID) { return CoreBase->_FindClass(ClassID); }
inline ERR AnalysePath(CSTRING Path, LOC *Type) { return CoreBase->_AnalysePath(Path,Type); }
inline ERR FreeResource(MEMORYID ID) { return CoreBase->_FreeResource(ID); }
inline CLASSID GetClassID(OBJECTID Object) { return CoreBase->_GetClassID(Object); }
inline OBJECTID GetOwnerID(OBJECTID Object) { return CoreBase->_GetOwnerID(Object); }
inline ERR CompareFilePaths(CSTRING PathA, CSTRING PathB) { return CoreBase->_CompareFilePaths(PathA,PathB); }
inline const struct SystemState * GetSystemState(void) { return CoreBase->_GetSystemState(); }
inline ERR ListChildren(OBJECTID Object, pf::vector<ChildEntry> *List) { return CoreBase->_ListChildren(Object,List); }
inline ERR RegisterFD(HOSTHANDLE FD, RFD Flags, void (*Routine)(HOSTHANDLE, APTR) , APTR Data) { return CoreBase->_RegisterFD(FD,Flags,Routine,Data); }
inline ERR ResolvePath(const std::string_view & Path, RSF Flags, std::string *Result) { return CoreBase->_ResolvePath(Path,Flags,Result); }
inline ERR MemoryIDInfo(MEMORYID ID, struct MemInfo *MemInfo, int Size) { return CoreBase->_MemoryIDInfo(ID,MemInfo,Size); }
inline ERR MemoryPtrInfo(APTR Address, struct MemInfo *MemInfo, int Size) { return CoreBase->_MemoryPtrInfo(Address,MemInfo,Size); }
inline ERR NewObject(CLASSID ClassID, NF Flags, OBJECTPTR *Object) { return CoreBase->_NewObject(ClassID,Flags,Object); }
inline void NotifySubscribers(OBJECTPTR Object, AC Action, APTR Args, ERR Error) { return CoreBase->_NotifySubscribers(Object,Action,Args,Error); }
inline ERR CopyFile(CSTRING Source, CSTRING Dest, FUNCTION *Callback) { return CoreBase->_CopyFile(Source,Dest,Callback); }
inline ERR ProcessMessages(PMF Flags, int TimeOut) { return CoreBase->_ProcessMessages(Flags,TimeOut); }
inline ERR IdentifyFile(CSTRING Path, CLASSID Filter, CLASSID *Class, CLASSID *SubClass) { return CoreBase->_IdentifyFile(Path,Filter,Class,SubClass); }
inline ERR ReallocMemory(APTR Memory, uint32_t Size, APTR *Address, MEMORYID *ID) { return CoreBase->_ReallocMemory(Memory,Size,Address,ID); }
inline ERR ReleaseMemory(MEMORYID MemoryID) { return CoreBase->_ReleaseMemory(MemoryID); }
inline CLASSID ResolveClassName(CSTRING Name) { return CoreBase->_ResolveClassName(Name); }
inline ERR SendMessage(MSGID Type, MSF Flags, APTR Data, int Size) { return CoreBase->_SendMessage(Type,Flags,Data,Size); }
inline ERR SetOwner(OBJECTPTR Object, OBJECTPTR Owner) { return CoreBase->_SetOwner(Object,Owner); }
inline ERR ProtectMemory(APTR Address, MEM Flags) { return CoreBase->_ProtectMemory(Address,Flags); }
inline void SetObjectContext(OBJECTPTR Object, struct Field *Field, AC ActionID) { return CoreBase->_SetObjectContext(Object,Field,ActionID); }
inline CSTRING FieldName(uint32_t FieldID) { return CoreBase->_FieldName(FieldID); }
inline ERR ScanDir(struct DirInfo *Info) { return CoreBase->_ScanDir(Info); }
inline ERR SetName(OBJECTPTR Object, CSTRING Name) { return CoreBase->_SetName(Object,Name); }
inline void LogReturn(void) { return CoreBase->_LogReturn(); }
inline ERR SubscribeAction(OBJECTPTR Object, AC Action, FUNCTION *Callback) { return CoreBase->_SubscribeAction(Object,Action,Callback); }
inline ERR SubscribeEvent(int64_t Event, FUNCTION *Callback, APTR *Handle) { return CoreBase->_SubscribeEvent(Event,Callback,Handle); }
inline ERR SubscribeTimer(double Interval, FUNCTION *Callback, APTR *Subscription) { return CoreBase->_SubscribeTimer(Interval,Callback,Subscription); }
inline ERR UpdateTimer(APTR Subscription, double Interval) { return CoreBase->_UpdateTimer(Subscription,Interval); }
inline ERR UnsubscribeAction(OBJECTPTR Object, AC Action) { return CoreBase->_UnsubscribeAction(Object,Action); }
inline void UnsubscribeEvent(APTR Handle) { return CoreBase->_UnsubscribeEvent(Handle); }
inline ERR BroadcastEvent(APTR Event, int EventSize) { return CoreBase->_BroadcastEvent(Event,EventSize); }
inline ERR WaitTime(double Seconds) { return CoreBase->_WaitTime(Seconds); }
inline int64_t GetEventID(EVG Group, CSTRING SubGroup, CSTRING Event) { return CoreBase->_GetEventID(Group,SubGroup,Event); }
inline uint32_t GenCRC32(uint32_t CRC, APTR Data, uint32_t Length) { return CoreBase->_GenCRC32(CRC,Data,Length); }
inline int64_t GetResource(RES Resource) { return CoreBase->_GetResource(Resource); }
inline int64_t SetResource(RES Resource, int64_t Value) { return CoreBase->_SetResource(Resource,Value); }
inline ERR ScanMessages(int *Handle, MSGID Type, APTR Buffer, int Size) { return CoreBase->_ScanMessages(Handle,Type,Buffer,Size); }
inline ERR WaitForObjects(PMF Flags, int TimeOut, struct ObjectSignal *ObjectSignals) { return CoreBase->_WaitForObjects(Flags,TimeOut,ObjectSignals); }
inline void UnloadFile(struct CacheFile *Cache) { return CoreBase->_UnloadFile(Cache); }
inline ERR CreateFolder(CSTRING Path, PERMIT Permissions) { return CoreBase->_CreateFolder(Path,Permissions); }
inline ERR LoadFile(CSTRING Path, LDF Flags, struct CacheFile **Cache) { return CoreBase->_LoadFile(Path,Flags,Cache); }
inline ERR SetVolume(CSTRING Name, CSTRING Path, CSTRING Icon, CSTRING Label, CSTRING Device, VOLUME Flags) { return CoreBase->_SetVolume(Name,Path,Icon,Label,Device,Flags); }
inline ERR DeleteVolume(CSTRING Name) { return CoreBase->_DeleteVolume(Name); }
inline ERR MoveFile(CSTRING Source, CSTRING Dest, FUNCTION *Callback) { return CoreBase->_MoveFile(Source,Dest,Callback); }
inline ERR UpdateMessage(int Message, MSGID Type, APTR Data, int Size) { return CoreBase->_UpdateMessage(Message,Type,Data,Size); }
inline ERR AddMsgHandler(MSGID MsgType, FUNCTION *Routine, struct MsgHandler **Handle) { return CoreBase->_AddMsgHandler(MsgType,Routine,Handle); }
inline ERR QueueAction(AC Action, OBJECTID Object, APTR Args) { return CoreBase->_QueueAction(Action,Object,Args); }
inline int64_t PreciseTime(void) { return CoreBase->_PreciseTime(); }
inline ERR OpenDir(CSTRING Path, RDF Flags, struct DirInfo **Info) { return CoreBase->_OpenDir(Path,Flags,Info); }
inline OBJECTPTR GetObjectPtr(OBJECTID Object) { return CoreBase->_GetObjectPtr(Object); }
inline struct Field * FindField(OBJECTPTR Object, uint32_t FieldID, OBJECTPTR *Target) { return CoreBase->_FindField(Object,FieldID,Target); }
inline CSTRING GetErrorMsg(ERR Error) { return CoreBase->_GetErrorMsg(Error); }
inline struct Message * GetActionMsg(void) { return CoreBase->_GetActionMsg(); }
inline ERR FuncError(CSTRING Header, ERR Error) { return CoreBase->_FuncError(Header,Error); }
inline ERR LockObject(OBJECTPTR Object, int MilliSeconds) { return CoreBase->_LockObject(Object,MilliSeconds); }
inline void ReleaseObject(OBJECTPTR Object) { return CoreBase->_ReleaseObject(Object); }
inline ERR AsyncAction(AC Action, OBJECTPTR Object, APTR Args, FUNCTION *Callback) { return CoreBase->_AsyncAction(Action,Object,Args,Callback); }
inline ERR AddInfoTag(struct FileInfo *Info, CSTRING Name, CSTRING Value) { return CoreBase->_AddInfoTag(Info,Name,Value); }
inline void SetDefaultPermissions(int User, int Group, PERMIT Permissions) { return CoreBase->_SetDefaultPermissions(User,Group,Permissions); }
inline void VLogF(VLF Flags, const char *Header, const char *Message, va_list Args) { return CoreBase->_VLogF(Flags,Header,Message,Args); }
inline ERR ReadInfoTag(struct FileInfo *Info, CSTRING Name, CSTRING *Value) { return CoreBase->_ReadInfoTag(Info,Name,Value); }
inline ERR SetResourcePath(RP PathType, CSTRING Path) { return CoreBase->_SetResourcePath(PathType,Path); }
inline objTask * CurrentTask(void) { return CoreBase->_CurrentTask(); }
inline CSTRING ResolveGroupID(int Group) { return CoreBase->_ResolveGroupID(Group); }
inline CSTRING ResolveUserID(int User) { return CoreBase->_ResolveUserID(User); }
inline ERR CreateLink(CSTRING From, CSTRING To) { return CoreBase->_CreateLink(From,To); }
inline OBJECTPTR ParentContext(void) { return CoreBase->_ParentContext(); }
inline void SetResourceMgr(APTR Address, struct ResourceManager *Manager) { return CoreBase->_SetResourceMgr(Address,Manager); }
inline ERR WakeThread(int Thread, int Stop) { return CoreBase->_WakeThread(Thread,Stop); }
inline ERR AsyncCancel(OBJECTID *Objects, int Size) { return CoreBase->_AsyncCancel(Objects,Size); }
inline int AsyncPending(OBJECTID Object) { return CoreBase->_AsyncPending(Object); }
inline ERR AsyncWait(OBJECTID *Objects, int Size, int TimeOut) { return CoreBase->_AsyncWait(Objects,Size,TimeOut); }
#else
extern "C" ERR AccessMemory(MEMORYID Memory, MEM Flags, int MilliSeconds, APTR *Result);
extern "C" ERR Action(AC Action, OBJECTPTR Object, APTR Parameters);
extern "C" void ActionList(struct ActionTable **Actions, int *Size);
extern "C" ERR DeleteFile(CSTRING Path, FUNCTION *Callback);
extern "C" CSTRING ResolveClassID(CLASSID ID);
extern "C" int AllocateID(IDTYPE Type);
extern "C" ERR AllocMemory(int Size, MEM Flags, APTR *Address, MEMORYID *ID);
extern "C" ERR AccessObject(OBJECTID Object, int MilliSeconds, OBJECTPTR *Result);
extern "C" ERR CheckAction(OBJECTPTR Object, AC Action);
extern "C" ERR CheckMemoryExists(MEMORYID ID);
extern "C" ERR CheckObjectExists(OBJECTID Object);
extern "C" ERR InitObject(OBJECTPTR Object);
extern "C" OBJECTPTR CurrentContext(void);
extern "C" int AdjustLogLevel(int Delta);
extern "C" ERR ReadFileToBuffer(CSTRING Path, APTR Buffer, int BufferSize, int *Result);
extern "C" ERR FindObject(CSTRING Name, CLASSID ClassID, FOF Flags, OBJECTID *ObjectID);
extern "C" objMetaClass * FindClass(CLASSID ClassID);
extern "C" ERR AnalysePath(CSTRING Path, LOC *Type);
extern "C" ERR FreeResource(MEMORYID ID);
extern "C" CLASSID GetClassID(OBJECTID Object);
extern "C" OBJECTID GetOwnerID(OBJECTID Object);
extern "C" ERR CompareFilePaths(CSTRING PathA, CSTRING PathB);
extern "C" const struct SystemState * GetSystemState(void);
extern "C" ERR ListChildren(OBJECTID Object, pf::vector<ChildEntry> *List);
extern "C" ERR RegisterFD(HOSTHANDLE FD, RFD Flags, void (*Routine)(HOSTHANDLE, APTR) , APTR Data);
extern "C" ERR ResolvePath(const std::string_view & Path, RSF Flags, std::string *Result);
extern "C" ERR MemoryIDInfo(MEMORYID ID, struct MemInfo *MemInfo, int Size);
extern "C" ERR MemoryPtrInfo(APTR Address, struct MemInfo *MemInfo, int Size);
extern "C" ERR NewObject(CLASSID ClassID, NF Flags, OBJECTPTR *Object);
extern "C" void NotifySubscribers(OBJECTPTR Object, AC Action, APTR Args, ERR Error);
extern "C" ERR CopyFile(CSTRING Source, CSTRING Dest, FUNCTION *Callback);
extern "C" ERR ProcessMessages(PMF Flags, int TimeOut);
extern "C" ERR IdentifyFile(CSTRING Path, CLASSID Filter, CLASSID *Class, CLASSID *SubClass);
extern "C" ERR ReallocMemory(APTR Memory, uint32_t Size, APTR *Address, MEMORYID *ID);
extern "C" ERR ReleaseMemory(MEMORYID MemoryID);
extern "C" CLASSID ResolveClassName(CSTRING Name);
extern "C" ERR SendMessage(MSGID Type, MSF Flags, APTR Data, int Size);
extern "C" ERR SetOwner(OBJECTPTR Object, OBJECTPTR Owner);
extern "C" ERR ProtectMemory(APTR Address, MEM Flags);
extern "C" void SetObjectContext(OBJECTPTR Object, struct Field *Field, AC ActionID);
extern "C" CSTRING FieldName(uint32_t FieldID);
extern "C" ERR ScanDir(struct DirInfo *Info);
extern "C" ERR SetName(OBJECTPTR Object, CSTRING Name);
extern "C" void LogReturn(void);
extern "C" ERR SubscribeAction(OBJECTPTR Object, AC Action, FUNCTION *Callback);
extern "C" ERR SubscribeEvent(int64_t Event, FUNCTION *Callback, APTR *Handle);
extern "C" ERR SubscribeTimer(double Interval, FUNCTION *Callback, APTR *Subscription);
extern "C" ERR UpdateTimer(APTR Subscription, double Interval);
extern "C" ERR UnsubscribeAction(OBJECTPTR Object, AC Action);
extern "C" void UnsubscribeEvent(APTR Handle);
extern "C" ERR BroadcastEvent(APTR Event, int EventSize);
extern "C" ERR WaitTime(double Seconds);
extern "C" int64_t GetEventID(EVG Group, CSTRING SubGroup, CSTRING Event);
extern "C" uint32_t GenCRC32(uint32_t CRC, APTR Data, uint32_t Length);
extern "C" int64_t GetResource(RES Resource);
extern "C" int64_t SetResource(RES Resource, int64_t Value);
extern "C" ERR ScanMessages(int *Handle, MSGID Type, APTR Buffer, int Size);
extern "C" ERR WaitForObjects(PMF Flags, int TimeOut, struct ObjectSignal *ObjectSignals);
extern "C" void UnloadFile(struct CacheFile *Cache);
extern "C" ERR CreateFolder(CSTRING Path, PERMIT Permissions);
extern "C" ERR LoadFile(CSTRING Path, LDF Flags, struct CacheFile **Cache);
extern "C" ERR SetVolume(CSTRING Name, CSTRING Path, CSTRING Icon, CSTRING Label, CSTRING Device, VOLUME Flags);
extern "C" ERR DeleteVolume(CSTRING Name);
extern "C" ERR MoveFile(CSTRING Source, CSTRING Dest, FUNCTION *Callback);
extern "C" ERR UpdateMessage(int Message, MSGID Type, APTR Data, int Size);
extern "C" ERR AddMsgHandler(MSGID MsgType, FUNCTION *Routine, struct MsgHandler **Handle);
extern "C" ERR QueueAction(AC Action, OBJECTID Object, APTR Args);
extern "C" int64_t PreciseTime(void);
extern "C" ERR OpenDir(CSTRING Path, RDF Flags, struct DirInfo **Info);
extern "C" OBJECTPTR GetObjectPtr(OBJECTID Object);
extern "C" struct Field * FindField(OBJECTPTR Object, uint32_t FieldID, OBJECTPTR *Target);
extern "C" CSTRING GetErrorMsg(ERR Error);
extern "C" struct Message * GetActionMsg(void);
extern "C" ERR FuncError(CSTRING Header, ERR Error);
extern "C" ERR LockObject(OBJECTPTR Object, int MilliSeconds);
extern "C" void ReleaseObject(OBJECTPTR Object);
extern "C" ERR AsyncAction(AC Action, OBJECTPTR Object, APTR Args, FUNCTION *Callback);
extern "C" ERR AddInfoTag(struct FileInfo *Info, CSTRING Name, CSTRING Value);
extern "C" void SetDefaultPermissions(int User, int Group, PERMIT Permissions);
extern "C" void VLogF(VLF Flags, const char *Header, const char *Message, va_list Args);
extern "C" ERR ReadInfoTag(struct FileInfo *Info, CSTRING Name, CSTRING *Value);
extern "C" ERR SetResourcePath(RP PathType, CSTRING Path);
extern "C" objTask * CurrentTask(void);
extern "C" CSTRING ResolveGroupID(int Group);
extern "C" CSTRING ResolveUserID(int User);
extern "C" ERR CreateLink(CSTRING From, CSTRING To);
extern "C" OBJECTPTR ParentContext(void);
extern "C" void SetResourceMgr(APTR Address, struct ResourceManager *Manager);
extern "C" ERR WakeThread(int Thread, int Stop);
extern "C" ERR AsyncCancel(OBJECTID *Objects, int Size);
extern "C" int AsyncPending(OBJECTID Object);
extern "C" ERR AsyncWait(OBJECTID *Objects, int Size, int TimeOut);
#endif // KOTUKU_STATIC


//********************************************************************************************************************

template <class T> inline MEMORYID GetMemoryID(T &&A) {
   return ((MEMORYID *)A)[-2];
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

inline ERR ReleaseMemory(const void *Address) {
   if (!Address) return ERR::NullArgs;
   return ReleaseMemory(((MEMORYID *)Address)[-2]);
}

inline ERR FreeResource(const void *Address) {
   if (!Address) return ERR::NullArgs;
   return FreeResource(((int *)Address)[-2]);
}

inline ERR AllocMemory(int Size, MEM Flags, APTR Address) {
   return AllocMemory(Size, Flags, (APTR *)Address, nullptr);
}

template<class T> inline ERR NewObject(CLASSID ClassID, T **Result) {
   return NewObject(ClassID, NF::NIL, (OBJECTPTR *)Result);
}

template<class T> inline ERR NewLocalObject(CLASSID ClassID, T **Result) {
   return NewObject(ClassID, NF::LOCAL, (OBJECTPTR *)Result);
}

inline ERR MemoryIDInfo(MEMORYID ID, struct MemInfo * MemInfo) {
   return MemoryIDInfo(ID,MemInfo,sizeof(struct MemInfo));
}

inline ERR MemoryPtrInfo(APTR Address, struct MemInfo * MemInfo) {
   return MemoryPtrInfo(Address,MemInfo,sizeof(struct MemInfo));
}

inline ERR QueueAction(AC Action, OBJECTID ObjectID) {
   return QueueAction(Action, ObjectID, nullptr);
}
#endif

typedef KEYVALUE ConfigKeys;
typedef std::pair<std::string, ConfigKeys> ConfigGroup;
typedef std::vector<ConfigGroup> ConfigGroups;

namespace pf {

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
struct FindField { int ID; struct Field * Field; objMetaClass * Source; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objMetaClass : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::METACLASS;
   static constexpr CSTRING CLASS_NAME = "MetaClass";

   using create = pf::Create<objMetaClass>;

   double  ClassVersion;                // The version number of the class.
   const struct FieldArray * Fields;    // Points to a FieldArray that describes the class' object structure.
   struct Field * Dictionary;           // Returns a field lookup table sorted by field IDs.
   CSTRING ClassName;                   // The name of the represented class.
   CSTRING FileExtension;               // Describes the file extension represented by the class.
   CSTRING FileDescription;             // Describes the file type represented by the class.
   CSTRING FileHeader;                  // Defines a string expression that will allow relevant file data to be matched to the class.
   CSTRING Path;                        // The path to the module binary that represents the class.
   CSTRING Icon;                        // Associates an icon with the file data for this class.
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
      return(error);
   }

   // Customised field setting

   inline ERR setClassVersion(const double Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->ClassVersion = Value;
      return ERR::Okay;
   }

   inline ERR setFields(const struct FieldArray * Value, int Elements) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[25];
      return field->WriteValue(target, field, 0x00001510, Value, Elements);
   }

   template <class T> inline ERR setClassName(T && Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->ClassName = Value;
      return ERR::Okay;
   }

   template <class T> inline ERR setFileExtension(T && Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->FileExtension = Value;
      return ERR::Okay;
   }

   template <class T> inline ERR setFileDescription(T && Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->FileDescription = Value;
      return ERR::Okay;
   }

   template <class T> inline ERR setFileHeader(T && Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->FileHeader = Value;
      return ERR::Okay;
   }

   template <class T> inline ERR setPath(T && Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Path = Value;
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

   inline ERR setMethods(const APTR Value, int Elements) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[20];
      return field->WriteValue(target, field, 0x00001510, Value, Elements);
   }

   inline ERR setActions(APTR Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      auto target = this;
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(target, field, 0x08000400, Value, 1);
   }

   template <class T> inline ERR setName(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(target, field, 0x08810500, to_cstring(Value), 1);
   }

};

inline bool Object::isSubClass() { return Class->ClassID != Class->BaseClassID; }
inline CLASSID Object::classID() { return Class->ClassID; }
inline CLASSID Object::baseClassID() { return Class->BaseClassID; }

// StorageDevice class definition

#define VER_STORAGEDEVICE (1.000000)

class objStorageDevice : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::STORAGEDEVICE;
   static constexpr CSTRING CLASS_NAME = "StorageDevice";

   using create = pf::Create<objStorageDevice>;

   DEVICE  DeviceFlags;   // These read-only flags identify the type of device and its features.
   int64_t DeviceSize;    // The storage size of the device in bytes, without accounting for the file system format.
   int64_t BytesFree;     // Total amount of storage space that is available, measured in bytes.
   int64_t BytesUsed;     // Total amount of storage space in use.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   template <class T> inline ERR setVolume(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(target, field, 0x08800500, to_cstring(Value), 1);
   }

};

// File class definition

#define VER_FILE (1.200000)

// File methods

namespace fl {
struct StartStream { OBJECTID SubscriberID; FL Flags; int Length; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct StopStream { static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Delete { FUNCTION * Callback; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Move { CSTRING Dest; FUNCTION * Callback; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Copy { CSTRING Dest; FUNCTION * Callback; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetDate { int Year; int Month; int Day; int Hour; int Minute; int Second; FDT Type; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ReadLine { STRING Result; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct BufferContent { static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Next { objFile * File; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Watch { FUNCTION * Callback; int64_t Custom; MFF Flags; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objFile : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::FILE;
   static constexpr CSTRING CLASS_NAME = "File";

   using create = pf::Create<objFile>;

   int64_t  Position;   // The current read/write byte position in a file.
   FL       Flags;      // File flags and options.
   int      Static;     // Set to true if a file object should be static.
   OBJECTID TargetID;   // Specifies a surface ID to target for user feedback and dialog boxes.
   int8_t * Buffer;     // Points to the internal data buffer if the file content is held in memory.
   public:
   inline CSTRING readLine() {
      struct fl::ReadLine args;
      if (Action(fl::ReadLine::id, this, &args) IS ERR::Okay) return args.Result;
      else return nullptr;
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
   inline ERR rename(CSTRING Name) noexcept {
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
   inline int writeResult(CPTR Buffer, int Size) noexcept {
      struct acWrite write = { (int8_t *)Buffer, Size };
      if (Action(AC::Write, this, &write) IS ERR::Okay) return write.Result;
      else return 0;
   }
   inline ERR startStream(OBJECTID SubscriberID, FL Flags, int Length) noexcept {
      struct fl::StartStream args = { SubscriberID, Flags, Length };
      return(Action(AC(-1), this, &args));
   }
   inline ERR stopStream() noexcept {
      return(Action(AC(-2), this, nullptr));
   }
   inline ERR del(FUNCTION Callback) noexcept {
      struct fl::Delete args = { &Callback };
      return(Action(AC(-3), this, &args));
   }
   inline ERR move(CSTRING Dest, FUNCTION Callback) noexcept {
      struct fl::Move args = { Dest, &Callback };
      return(Action(AC(-4), this, &args));
   }
   inline ERR copy(CSTRING Dest, FUNCTION Callback) noexcept {
      struct fl::Copy args = { Dest, &Callback };
      return(Action(AC(-5), this, &args));
   }
   inline ERR setDate(int Year, int Month, int Day, int Hour, int Minute, int Second, FDT Type) noexcept {
      struct fl::SetDate args = { Year, Month, Day, Hour, Minute, Second, Type };
      return(Action(AC(-6), this, &args));
   }
   inline ERR readLine(STRING * Result) noexcept {
      struct fl::ReadLine args = { (STRING)0 };
      ERR error = Action(AC(-7), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }
   inline ERR bufferContent() noexcept {
      return(Action(AC(-8), this, nullptr));
   }
   inline ERR next(objFile ** File) noexcept {
      struct fl::Next args = { (objFile *)0 };
      ERR error = Action(AC(-9), this, &args);
      if (File) *File = args.File;
      return(error);
   }
   inline ERR watch(FUNCTION Callback, int64_t Custom, MFF Flags) noexcept {
      struct fl::Watch args = { &Callback, Custom, Flags };
      return(Action(AC(-10), this, &args));
   }

   // Customised field setting

   inline ERR setPosition(const int64_t Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(target, field, FD_INT64, &Value, 1);
   }

   inline ERR setFlags(const FL Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setStatic(const int Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Static = Value;
      return ERR::Okay;
   }

   inline ERR setTarget(OBJECTID Value) noexcept {
      this->TargetID = Value;
      return ERR::Okay;
   }

   inline ERR setDate(APTR Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(target, field, 0x08000310, Value, 1);
   }

   inline ERR setCreated(APTR Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[21];
      return field->WriteValue(target, field, 0x08000310, Value, 1);
   }

   template <class T> inline ERR setPath(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(target, field, 0x08800500, to_cstring(Value), 1);
   }

   inline ERR setPermissions(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[23];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setSize(const int64_t Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[18];
      return field->WriteValue(target, field, FD_INT64, &Value, 1);
   }

   template <class T> inline ERR setLink(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setUser(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setGroup(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

};

// Config class definition

#define VER_CONFIG (1.000000)

// Config methods

namespace cfg {
struct ReadValue { CSTRING Group; CSTRING Key; CSTRING Data; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Set { CSTRING Group; CSTRING Key; CSTRING Data; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct WriteValue { CSTRING Group; CSTRING Key; CSTRING Data; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DeleteKey { CSTRING Group; CSTRING Key; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DeleteGroup { CSTRING Group; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetGroupFromIndex { int Index; CSTRING Group; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SortByKey { CSTRING Key; int Descending; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct MergeFile { CSTRING Path; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Merge { OBJECTPTR Source; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objConfig : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::CONFIG;
   static constexpr CSTRING CLASS_NAME = "Config";

   using create = pf::Create<objConfig>;

   STRING Path;         // Set this field to the location of the source configuration file.
   STRING KeyFilter;    // Set this field to enable key filtering.
   STRING GroupFilter;  // Set this field to enable group filtering.
   CNF    Flags;        // Optional flags may be set here.
   public:
   ConfigGroups *Groups;

   // For C++ only, these read variants avoid method calls for speed, but apply identical logic.

   inline ERR read(std::string_view pGroup, std::string_view pKey, double &pValue) {
      for (auto& [group, keys] : Groups[0]) {
         if ((!pGroup.empty()) and (group.compare(pGroup))) continue;
         if (pKey.empty()) pValue = strtod(keys.cbegin()->second.c_str(), nullptr);
         else if (auto it = keys.find(pKey); it != keys.end()) pValue = strtod(it->second.c_str(), nullptr);
         else return ERR::Search;
         return ERR::Okay;
      }
      return ERR::Search;
   }

   inline ERR read(std::string_view pGroup, std::string_view pKey, int &pValue) {
      for (auto& [group, keys] : Groups[0]) {
         if ((!pGroup.empty()) and (group.compare(pGroup))) continue;
         if (pKey.empty()) pValue = strtol(keys.cbegin()->second.c_str(), nullptr, 0);
         else if (auto it = keys.find(pKey); it != keys.end()) pValue = strtol(it->second.c_str(), nullptr, 0);
         else return ERR::Search;
         return ERR::Okay;
      }
      return ERR::Search;
   }

   inline ERR read(std::string_view pGroup, std::string_view pKey, std::string &pValue) {
      for (auto & [group, keys] : Groups[0]) {
         if ((!pGroup.empty()) and (group.compare(pGroup))) continue;
         if (pKey.empty()) pValue = keys.cbegin()->second;
         else if (auto it = keys.find(pKey); it != keys.end()) pValue = it->second;
         else return ERR::Search;
         return ERR::Okay;
      }
      return ERR::Search;
   }

   inline ERR write(std::string_view Group, std::string_view Key, std::string_view Value) {
      ConfigGroups &groups = *Groups;
      for (auto& [group, keys] : groups) {
         if (!group.compare(Group)) {
            if (auto it = keys.find(Key); it != keys.end()) {
               it->second.assign(Value);
            }
            else keys.emplace(Key, Value);
            return ERR::Okay;
         }
      }

      auto &new_group = Groups->emplace_back();
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
   inline ERR readValue(CSTRING Group, CSTRING Key, CSTRING * Data) noexcept {
      struct cfg::ReadValue args = { Group, Key, (CSTRING)0 };
      ERR error = Action(AC(-1), this, &args);
      if (Data) *Data = args.Data;
      return(error);
   }
   inline ERR set(CSTRING Group, CSTRING Key, CSTRING Data) noexcept {
      struct cfg::Set args = { Group, Key, Data };
      return(Action(AC(-2), this, &args));
   }
   inline ERR writeValue(CSTRING Group, CSTRING Key, CSTRING Data) noexcept {
      struct cfg::WriteValue args = { Group, Key, Data };
      return(Action(AC(-3), this, &args));
   }
   inline ERR deleteKey(CSTRING Group, CSTRING Key) noexcept {
      struct cfg::DeleteKey args = { Group, Key };
      return(Action(AC(-4), this, &args));
   }
   inline ERR deleteGroup(CSTRING Group) noexcept {
      struct cfg::DeleteGroup args = { Group };
      return(Action(AC(-5), this, &args));
   }
   inline ERR getGroupFromIndex(int Index, CSTRING * Group) noexcept {
      struct cfg::GetGroupFromIndex args = { Index, (CSTRING)0 };
      ERR error = Action(AC(-6), this, &args);
      if (Group) *Group = args.Group;
      return(error);
   }
   inline ERR sortByKey(CSTRING Key, int Descending) noexcept {
      struct cfg::SortByKey args = { Key, Descending };
      return(Action(AC(-7), this, &args));
   }
   inline ERR mergeFile(CSTRING Path) noexcept {
      struct cfg::MergeFile args = { Path };
      return(Action(AC(-9), this, &args));
   }
   inline ERR merge(OBJECTPTR Source) noexcept {
      struct cfg::Merge args = { Source };
      return(Action(AC(-10), this, &args));
   }

   // Customised field setting

   template <class T> inline ERR setPath(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setKeyFilter(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setGroupFilter(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
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
struct Exec { CSTRING Procedure; const struct ScriptArg * Args; int TotalArgs; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DerefProcedure { FUNCTION * Procedure; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Callback { int64_t ProcedureID; const struct ScriptArg * Args; int TotalArgs; ERR Error; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetProcedureID { CSTRING Procedure; int64_t ProcedureID; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DebugLog { CSTRING Options; CSTRING Result; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objScript : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::SCRIPT;
   static constexpr CSTRING CLASS_NAME = "Script";

   using create = pf::Create<objScript>;

   OBJECTID TargetID;  // Reference to the default container that new script objects will be initialised to.
   SCF      Flags;     // Optional flags.
   ERR      Error;     // If a script fails during execution, an error code may be readable here.
   int      CurrentLine; // Indicates the current line being executed when in debug mode.
   int      LineOffset; // For debugging purposes, this value is added to any message referencing a line number.

#ifdef PRV_SCRIPT
   int64_t    ProcedureID;          // For callbacks
   KEYVALUE Vars; // Global parameters
   STRING   *Results;
   char     Language[4];          // 3-character language code, null-terminated
   const ScriptArg *ProcArgs;     // Procedure args - applies during Exec
   STRING   Path;                 // File location of the script
   STRING   String;
   STRING   WorkingPath;
   STRING   ErrorMessage;
   CSTRING  Procedure;
   STRING   CacheFile;
   int      ActivationCount;      // Incremented every time the script is activated.
   int      ResultsTotal;
   int      TotalArgs;            // Total number of ProcArgs
   char     LanguageDir[32];      // Directory to use for language files
   OBJECTID ScriptOwnerID;
#endif

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, const void *Buffer, int Size) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer, Size };
      return Action(AC::DataFeed, this, &args);
   }
   inline ERR getKey(CSTRING Key, STRING Value, int Size) noexcept {
      struct acGetKey args = { Key, Value, Size };
      auto error = Action(AC::GetKey, this, &args);
      if ((error != ERR::Okay) and (Value)) Value[0] = 0;
      return error;
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR reset() noexcept { return Action(AC::Reset, this, nullptr); }
   inline ERR acSetKey(CSTRING FieldName, CSTRING Value) noexcept {
      struct acSetKey args = { FieldName, Value };
      return Action(AC::SetKey, this, &args);
   }
   inline ERR exec(CSTRING Procedure, const struct ScriptArg * Args, int TotalArgs) noexcept {
      struct sc::Exec args = { Procedure, Args, TotalArgs };
      return(Action(AC(-1), this, &args));
   }
   inline ERR derefProcedure(FUNCTION Procedure) noexcept {
      struct sc::DerefProcedure args = { &Procedure };
      return(Action(AC(-2), this, &args));
   }
   inline ERR callback(int64_t ProcedureID, const struct ScriptArg * Args, int TotalArgs, ERR * Error) noexcept {
      struct sc::Callback args = { ProcedureID, Args, TotalArgs, (ERR)0 };
      ERR error = Action(AC(-3), this, &args);
      if (Error) *Error = args.Error;
      return(error);
   }
   inline ERR getProcedureID(CSTRING Procedure, int64_t * ProcedureID) noexcept {
      struct sc::GetProcedureID args = { Procedure, (int64_t)0 };
      ERR error = Action(AC(-4), this, &args);
      if (ProcedureID) *ProcedureID = args.ProcedureID;
      return(error);
   }
   inline ERR debugLog(CSTRING Options, CSTRING * Result) noexcept {
      struct sc::DebugLog args = { Options, (CSTRING)0 };
      ERR error = Action(AC(-5), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }

   // Customised field setting

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

   template <class T> inline ERR setCacheFile(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setErrorMessage(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[18];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setWorkingPath(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[21];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setProcedure(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setName(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(target, field, 0x08810300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setPath(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(target, field, 0x08800500, to_cstring(Value), 1);
   }

   inline ERR setResults(STRING * Value, int Elements) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(target, field, 0x08801300, Value, Elements);
   }

   template <class T> inline ERR setStatement(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
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
struct AddArgument { CSTRING Argument; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Quit { static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetEnv { CSTRING Name; CSTRING Value; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetEnv { CSTRING Name; CSTRING Value; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objTask : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::TASK;
   static constexpr CSTRING CLASS_NAME = "Task";

   using create = pf::Create<objTask>;

   double TimeOut;    // Limits the amount of time to wait for a launched process to return.
   TSF    Flags;      // Optional flags.
   int    ReturnCode; // The task's return code can be retrieved following execution.
   int    ProcessID;  // Reflects the process ID when an executable is launched.

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR getKey(CSTRING Key, STRING Value, int Size) noexcept {
      struct acGetKey args = { Key, Value, Size };
      auto error = Action(AC::GetKey, this, &args);
      if ((error != ERR::Okay) and (Value)) Value[0] = 0;
      return error;
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR acSetKey(CSTRING FieldName, CSTRING Value) noexcept {
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
   inline int writeResult(CPTR Buffer, int Size) noexcept {
      struct acWrite write = { (int8_t *)Buffer, Size };
      if (Action(AC::Write, this, &write) IS ERR::Okay) return write.Result;
      else return 0;
   }
   inline ERR expunge() noexcept {
      return(Action(AC(-1), this, nullptr));
   }
   inline ERR addArgument(CSTRING Argument) noexcept {
      struct task::AddArgument args = { Argument };
      return(Action(AC(-2), this, &args));
   }
   inline ERR quit() noexcept {
      return(Action(AC(-3), this, nullptr));
   }
   inline ERR getEnv(CSTRING Name, CSTRING * Value) noexcept {
      struct task::GetEnv args = { Name, (CSTRING)0 };
      ERR error = Action(AC(-4), this, &args);
      if (Value) *Value = args.Value;
      return(error);
   }
   inline ERR setEnv(CSTRING Name, CSTRING Value) noexcept {
      struct task::SetEnv args = { Name, Value };
      return(Action(AC(-5), this, &args));
   }

   // Customised field setting

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
      auto target = this;
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setProcess(const int Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->ProcessID = Value;
      return ERR::Okay;
   }

   inline ERR setAffinityMask(const int64_t Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(target, field, FD_INT64, &Value, 1);
   }

   template <class T> inline ERR setArgs(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(target, field, 0x08800200, to_cstring(Value), 1);
   }

   inline ERR setParameters(pf::vector<std::string> *Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(target, field, 0x08805300, Value, int(Value->size()));
   }

   inline ERR setErrorCallback(FUNCTION Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[18];
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   inline ERR setExitCallback(FUNCTION Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[20];
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   inline ERR setInputCallback(FUNCTION Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   template <class T> inline ERR setLaunchPath(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setLocation(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setName(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setOutputCallback(FUNCTION Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   template <class T> inline ERR setPath(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setPriority(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
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

   using create = pf::Create<objThread>;

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
      return(Action(AC(-1), this, &args));
   }

   // Customised field setting

   inline ERR setFlags(const THF Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setCallback(FUNCTION Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   inline ERR setRoutine(FUNCTION Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

};

// Module class definition

#define VER_MODULE (1.000000)

// Module methods

namespace mod {
struct ResolveSymbol { CSTRING Name; APTR Address; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Test { CSTRING Options; int Passed; int Total; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objModule : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::MODULE;
   static constexpr CSTRING CLASS_NAME = "Module";

   using create = pf::Create<objModule>;

   const struct Function * FunctionList;    // Refers to a list of public functions exported by the module.
   APTR ModBase;                            // The Module's function base (jump table) must be read from this field.
   class RootModule * Root;                 // For internal use only.
   struct ModHeader * Header;               // For internal usage only.
   MOF  Flags;                              // Optional flags.
   public:
   static ERR load(std::string Name, OBJECTPTR *Module = nullptr, APTR Functions = nullptr) {
      if (auto module = objModule::create::global(pf::FieldValue(FID_Name, Name.c_str()))) {
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
   inline ERR resolveSymbol(CSTRING Name, APTR * Address) noexcept {
      struct mod::ResolveSymbol args = { Name, (APTR)0 };
      ERR error = Action(AC(-1), this, &args);
      if (Address) *Address = args.Address;
      return(error);
   }
   inline ERR test(CSTRING Options, int * Passed, int * Total) noexcept {
      struct mod::Test args = { Options, (int)0, (int)0 };
      ERR error = Action(AC(-2), this, &args);
      if (Passed) *Passed = args.Passed;
      if (Total) *Total = args.Total;
      return(error);
   }

   // Customised field setting

   inline ERR setFunctionList(const struct Function * Value) noexcept {
      this->FunctionList = Value;
      return ERR::Okay;
   }

   inline ERR setHeader(struct ModHeader * Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(target, field, 0x08000510, Value, 1);
   }

   inline ERR setFlags(const MOF Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Flags = Value;
      return ERR::Okay;
   }

   template <class T> inline ERR setName(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(target, field, 0x08800500, to_cstring(Value), 1);
   }

};

// Time class definition

#define VER_TIME (1.000000)

// Time methods

namespace pt {
struct SetTime { static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objTime : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::TIME;
   static constexpr CSTRING CLASS_NAME = "Time";

   using create = pf::Create<objTime>;

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
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR setTime() noexcept {
      return(Action(AC(-1), this, nullptr));
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
struct CompressFile { CSTRING Location; CSTRING Path; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressBuffer { APTR Input; APTR Output; int OutputSize; int Result; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressFile { CSTRING Path; CSTRING Dest; int Flags; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RemoveFile { CSTRING Path; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CompressStream { APTR Input; int Length; FUNCTION * Callback; APTR Output; int OutputSize; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressStream { APTR Input; int Length; FUNCTION * Callback; APTR Output; int OutputSize; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CompressStreamStart { static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CompressStreamEnd { FUNCTION * Callback; APTR Output; int OutputSize; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressStreamEnd { FUNCTION * Callback; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressStreamStart { static const AC id = AC(-11); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressObject { CSTRING Path; OBJECTPTR Object; static const AC id = AC(-12); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Scan { CSTRING Folder; CSTRING Filter; FUNCTION * Callback; static const AC id = AC(-13); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Find { CSTRING Path; int CaseSensitive; int Wildcard; struct CompressedItem * Item; static const AC id = AC(-14); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objCompression : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::COMPRESSION;
   static constexpr CSTRING CLASS_NAME = "Compression";

   using create = pf::Create<objCompression>;

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
      return(error);
   }
   inline ERR compressFile(CSTRING Location, CSTRING Path) noexcept {
      struct cmp::CompressFile args = { Location, Path };
      return(Action(AC(-2), this, &args));
   }
   inline ERR decompressBuffer(APTR Input, APTR Output, int OutputSize, int * Result) noexcept {
      struct cmp::DecompressBuffer args = { Input, Output, OutputSize, (int)0 };
      ERR error = Action(AC(-3), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }
   inline ERR decompressFile(CSTRING Path, CSTRING Dest, int Flags) noexcept {
      struct cmp::DecompressFile args = { Path, Dest, Flags };
      return(Action(AC(-4), this, &args));
   }
   inline ERR removeFile(CSTRING Path) noexcept {
      struct cmp::RemoveFile args = { Path };
      return(Action(AC(-5), this, &args));
   }
   inline ERR compressStream(APTR Input, int Length, FUNCTION Callback, APTR Output, int OutputSize) noexcept {
      struct cmp::CompressStream args = { Input, Length, &Callback, Output, OutputSize };
      return(Action(AC(-6), this, &args));
   }
   inline ERR decompressStream(APTR Input, int Length, FUNCTION Callback, APTR Output, int OutputSize) noexcept {
      struct cmp::DecompressStream args = { Input, Length, &Callback, Output, OutputSize };
      return(Action(AC(-7), this, &args));
   }
   inline ERR compressStreamStart() noexcept {
      return(Action(AC(-8), this, nullptr));
   }
   inline ERR compressStreamEnd(FUNCTION Callback, APTR Output, int OutputSize) noexcept {
      struct cmp::CompressStreamEnd args = { &Callback, Output, OutputSize };
      return(Action(AC(-9), this, &args));
   }
   inline ERR decompressStreamEnd(FUNCTION Callback) noexcept {
      struct cmp::DecompressStreamEnd args = { &Callback };
      return(Action(AC(-10), this, &args));
   }
   inline ERR decompressStreamStart() noexcept {
      return(Action(AC(-11), this, nullptr));
   }
   inline ERR decompressObject(CSTRING Path, OBJECTPTR Object) noexcept {
      struct cmp::DecompressObject args = { Path, Object };
      return(Action(AC(-12), this, &args));
   }
   inline ERR scan(CSTRING Folder, CSTRING Filter, FUNCTION Callback) noexcept {
      struct cmp::Scan args = { Folder, Filter, &Callback };
      return(Action(AC(-13), this, &args));
   }
   inline ERR find(CSTRING Path, int CaseSensitive, int Wildcard, struct CompressedItem ** Item) noexcept {
      struct cmp::Find args = { Path, CaseSensitive, Wildcard, (struct CompressedItem *)0 };
      ERR error = Action(AC(-14), this, &args);
      if (Item) *Item = args.Item;
      return(error);
   }

   // Customised field setting

   inline ERR setOutput(OBJECTID Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->OutputID = Value;
      return ERR::Okay;
   }

   inline ERR setCompressionLevel(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
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
      auto target = this;
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   template <class T> inline ERR setArchiveName(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(target, field, 0x08800200, to_cstring(Value), 1);
   }

   template <class T> inline ERR setPath(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setFeedback(FUNCTION Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[18];
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   template <class T> inline ERR setPassword(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

};

// CompressedStream class definition

#define VER_COMPRESSEDSTREAM (1.000000)

class objCompressedStream : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::COMPRESSEDSTREAM;
   static constexpr CSTRING CLASS_NAME = "CompressedStream";

   using create = pf::Create<objCompressedStream>;

   int64_t   TotalOutput;  // A live counter of total bytes that have been output by the stream.
   OBJECTPTR Input;        // An input object that will supply data for decompression.
   OBJECTPTR Output;       // A target object that will receive data compressed by the stream.
   CF        Format;       // The format of the compressed stream.  The default is GZIP.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

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

#ifndef PRV_CORE

// Note that the length of the data is only needed when messaging between processes, so we can skip it for these
// direct-access data channel macros.

#define acDataContent(a,b)  acDataFeed((a),0,DATA::CONTENT,(b),0)
#define acDataXML(a,b)      acDataFeed((a),0,DATA::XML,(b),0)
#define acDataText(a,b)     acDataFeed((a),0,DATA::TEXT,(b),0)

#endif // PRV_CORE

#ifdef __unix__
#include <pthread.h>
#endif

namespace pf {

#ifdef __system__
   struct ActionMessage {
      OBJECTID ObjectID;  // The object that is to receive the action
      int  Time;
      AC ActionID;        // ID of the action or method to execute
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
   char  ID[20];         // Typically the PCI bus ID or USB bus ID, serial number or unique identifier
   char  Group[32];      // Group name in the config file
   char  Class[32];      // Class identifier (USB)
   union {
      char Product[40];  // Name of product or the hardware device
      char Device[40];
   };
   char Vendor[40];      // Name of vendor
};

// Speed efficient way of setting a string field that is managed with AllocMemory().

inline ERR set_string_field(const std::string_view Source, STRING &Dest)
{
   MemInfo info;
   if (auto error = MemoryIDInfo(GetMemoryID(Dest), &info, sizeof(info)); error IS ERR::Okay) {
      if (Source.size()+1 < info.Size) {
         copymem(Source.data(), Dest, Source.size());
         Dest[Source.size()] = 0;
         return ERR::Okay;
      }
      else {
         FreeResource(GetMemoryID(Dest));
         if (AllocMemory(Source.size() + 1, MEM::STRING|MEM::NO_CLEAR, (APTR *)&Dest, nullptr) IS ERR::Okay) {
            copymem(Source.data(), Dest, Source.size());
            Dest[Source.size()] = 0;
            return ERR::Okay;
         }
         else return ERR::AllocMemory;
      }
   }
   else return error;
}

[[nodiscard]] inline char * strclone(const std::string_view String) noexcept
{
   char *newstr;
   if (AllocMemory(String.size()+1, MEM::STRING|MEM::NO_CLEAR, (APTR *)&newstr, nullptr) IS ERR::Okay) {
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

inline CSTRING Object::className() { return Class->ClassName; }
