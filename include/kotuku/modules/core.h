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
#include <span>
#include <charconv>
#include <sstream>
#include <cmath>
#include <type_traits>
#include "ankerl/unordered_dense.h"
#endif

class objMetaClass;

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
   PLACEMENT = 0x00000800,
   ZOMBIE = 0x00001000,
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
   PROCESS_STATE = 12,
   TOTAL_MEMORY = 13,
   TOTAL_SWAP = 14,
   CPU_SPEED = 15,
   FREE_MEMORY = 16,
   MEMORY_USAGE = 17,
   MAIN_THREAD = 18,
   MAIN_THREAD_ID = 19,
   WINDOWS_ICON = 20,
   STRUCT_DB = 21,
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

struct FunctionField {
   CSTRING  Name;   // Name of the field
   uint32_t Type;   // Type of the field
};

struct ActionEntry {
   ERR (*PerformAction)(OBJECTPTR, APTR);     // Pointer to a custom action hook.
};


typedef AC ACTIONID;

struct StructInfo {
   uint16_t Size;       // Byte size of the structure (sizeof)
   uint16_t Alignment;  // Alignment requirement of the structure (alignof)
};

using LOG_CALLBACK = void(*)(CSTRING Header, CSTRING Message, int Depth, int MsgLevel, int LogLevel);

constexpr RESOURCEID RESOURCEID_INHERIT = 1;

#ifdef _LP64
#define FD_PTR64 FD_POINTER
#else
#define FD_PTR64 0
#endif

// Forward declared classes

class objScript;
class objConfig;
class objCompression;
class objCompressedStream;
class objFile;
class objModule;
class objStorageDevice;
class objThread;
class objTask;


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

struct ResourceRecord {
   APTR     Address;                    // Direct pointer to the resource (optional, can rely on ResourceID instead)
   struct ResourceManager * Manager;    // Reference to the resource manager for this record
   RESOURCEID ResourceID;               // Unique identifier
   OBJECTID OwnerID;                    // Owner of the resource
   bool     CollectOnUnlock;            // Resource is locked; manager will collect immediately once unlocked
   bool     Terminating;                // A FreeResource() call currently owns the destruction path
   bool     OwnerManagesChildren;       // True if the current OwnerID manages its child resources
   ResourceRecord() :
      Address(nullptr), Manager(nullptr), ResourceID(0), OwnerID(0), CollectOnUnlock(false),
      Terminating(false), OwnerManagesChildren(false) { };

   ResourceRecord(RESOURCEID pResourceID, APTR pAddress, OBJECTID pOwnerID, ResourceManager *pManager) :
      Address(pAddress), Manager(pManager), ResourceID(pResourceID), OwnerID(pOwnerID), CollectOnUnlock(false),
      Terminating(false), OwnerManagesChildren(false) { };
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
   static constexpr double DefaultDPI = 96.0;
   static constexpr double DefaultFontSize = 16.0;
   // Note that the type holds optional FD metadata; the use of FD_DOUBLE is unnecessary
   constexpr Unit(double pValue, int pType = 0) : Value(pValue), Type(pType) { }
   constexpr Unit() : Value(std::numeric_limits<double>::quiet_NaN()), Type(0) { }
   explicit Unit(std::string_view String, double FontSize = DefaultFontSize, double DPI = DefaultDPI) :
      Value(std::numeric_limits<double>::quiet_NaN()), Type(0) { read(String, FontSize, DPI); }

   constexpr operator double() const { return Value; }
   constexpr int round() const { return int(Value + 0.5); }

   double operator*() const { return Value; };

   constexpr void set(const double pValue) { Value = pValue; }
   constexpr bool scaled() const { return (Type & FD_SCALED) ? true : false; }
   constexpr bool verbatim() const { return (Type & FD_PURE) ? true : false; }
   inline bool defined() const { return !std::isnan(Value); } // A NaN value denotes an undefined unit

   void read(std::string_view String, double FontSize = DefaultFontSize, double DPI = DefaultDPI) {
      Type = 0;
      const auto start = String.find_first_not_of(" \n\r\t");
      if (start != std::string::npos) String.remove_prefix(start);
      if (String.starts_with('+')) String.remove_prefix(1);
      auto [ end, error ] = std::from_chars(String.data(), String.data() + String.size(), Value);
      if (error != std::errc()) { Value = std::numeric_limits<double>::quiet_NaN(); return; }

      String = String.substr(end - String.data());
      if (String.starts_with("%")) { Value *= 0.01; Type = FD_SCALED; }
      else if (String.starts_with("em")) Value *= FontSize;
      else if (String.starts_with("ex")) Value *= FontSize * 0.5;
      else if (String.starts_with("in")) Value *= DPI;
      else if (String.starts_with("cm")) Value *= (1.0 / 2.54) * DPI;
      else if (String.starts_with("mm")) Value *= (1.0 / 25.4) * DPI;
      else if (String.starts_with("pt")) Value *= (4.0 / 3.0);
      else if (String.starts_with("pc")) Value *= (4.0 / 3.0) * 12.0;
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
   MAXINT   Arg;                                                              // An option to complement the field type.  Can be a pointer or an integer value
   ERR (*GetValue)(APTR, APTR);                                               // A virtual function that will retrieve the value for this field
   APTR     SetValue;                                                         // A virtual function that will set the value for this field
   ERR (*WriteValue)(OBJECTPTR, const struct Field *, int, const void *);     // An internal function for writing to this field
   CSTRING  Name;                                                             // The English name for the field, e.g. Width
   uint32_t FieldID;                                                          // 32-bit hash from fieldhash()
   uint16_t Offset;                                                           // Field offset within the object
   uint16_t Index;                                                            // Field array index
   uint32_t Flags;                                                            // Special flags that describe the field
   inline bool readable() const { return (Flags & FD_READ) ? true : false; }
   inline bool writeable() const { return (Flags & (FD_WRITE|FD_INIT)) ? true : false; }
   inline bool pure() const { return (Flags & FD_PURE) ? true : false; }
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
   void (*_ReleaseZombie)(OBJECTPTR Object);
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
   void (*_SetObjectContext)(OBJECTPTR Object, const struct Field *Field, AC ActionID);
   CSTRING (*_FieldName)(uint32_t FieldID);
   ERR (*_ScanDir)(struct DirInfo *Info);
   ERR (*_SetName)(OBJECTPTR Object, const std::string_view &Name);
   void (*_LogReturn)(void);
   ERR (*_SubscribeAction)(OBJECTPTR Object, AC Action, FUNCTION *Callback);
   ERR (*_SubscribeEvent)(int64_t Event, FUNCTION *Callback, APTR *Handle);
   ERR (*_SubscribeTimer)(double Interval, FUNCTION *Callback, APTR *Subscription);
   ERR (*_UpdateTimer)(APTR Subscription, double Interval);
   ERR (*_UnsubscribeAction)(OBJECTPTR Object, AC Action, FUNCTION *Callback);
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
   const struct Field * (*_FindField)(OBJECTPTR Object, uint32_t FieldID, OBJECTPTR *Target);
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
   int (*_GetThreadID)(void);
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
inline void ReleaseZombie(OBJECTPTR Object) { return CoreBase->_ReleaseZombie(Object); }
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
inline void SetObjectContext(OBJECTPTR Object, const struct Field *Field, AC ActionID) { return CoreBase->_SetObjectContext(Object,Field,ActionID); }
inline CSTRING FieldName(uint32_t FieldID) { return CoreBase->_FieldName(FieldID); }
inline ERR ScanDir(struct DirInfo *Info) { return CoreBase->_ScanDir(Info); }
inline ERR SetName(OBJECTPTR Object, const std::string_view &Name) { return CoreBase->_SetName(Object,Name); }
inline void LogReturn(void) { return CoreBase->_LogReturn(); }
inline ERR SubscribeAction(OBJECTPTR Object, AC Action, FUNCTION *Callback) { return CoreBase->_SubscribeAction(Object,Action,Callback); }
inline ERR SubscribeEvent(int64_t Event, FUNCTION *Callback, APTR *Handle) { return CoreBase->_SubscribeEvent(Event,Callback,Handle); }
inline ERR SubscribeTimer(double Interval, FUNCTION *Callback, APTR *Subscription) { return CoreBase->_SubscribeTimer(Interval,Callback,Subscription); }
inline ERR UpdateTimer(APTR Subscription, double Interval) { return CoreBase->_UpdateTimer(Subscription,Interval); }
inline ERR UnsubscribeAction(OBJECTPTR Object, AC Action, FUNCTION *Callback) { return CoreBase->_UnsubscribeAction(Object,Action,Callback); }
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
inline const struct Field * FindField(OBJECTPTR Object, uint32_t FieldID, OBJECTPTR *Target) { return CoreBase->_FindField(Object,FieldID,Target); }
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
inline int GetThreadID(void) { return CoreBase->_GetThreadID(); }
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
extern "C" void ReleaseZombie(OBJECTPTR Object);
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
extern "C" void SetObjectContext(OBJECTPTR Object, const struct Field *Field, AC ActionID);
extern "C" CSTRING FieldName(uint32_t FieldID);
extern "C" ERR ScanDir(struct DirInfo *Info);
extern "C" ERR SetName(OBJECTPTR Object, const std::string_view &Name);
extern "C" void LogReturn(void);
extern "C" ERR SubscribeAction(OBJECTPTR Object, AC Action, FUNCTION *Callback);
extern "C" ERR SubscribeEvent(int64_t Event, FUNCTION *Callback, APTR *Handle);
extern "C" ERR SubscribeTimer(double Interval, FUNCTION *Callback, APTR *Subscription);
extern "C" ERR UpdateTimer(APTR Subscription, double Interval);
extern "C" ERR UnsubscribeAction(OBJECTPTR Object, AC Action, FUNCTION *Callback);
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
extern "C" const struct Field * FindField(OBJECTPTR Object, uint32_t FieldID, OBJECTPTR *Target);
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
extern "C" int GetThreadID(void);
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

[[nodiscard]] constexpr inline CSTRING to_cstring(CSTRING A) { return A; }
[[nodiscard]] inline CSTRING to_cstring(const std::string &A) { return A.c_str(); }

#ifndef PRV_CORE_DATA
// These overloaded functions can't be used in the Core as they will confuse the compiler in key areas.

inline ERR SubscribeAction(OBJECTPTR Object, AC Action, FUNCTION Callback) {
   return SubscribeAction(Object, Action, &Callback);
}

inline ERR SubscribeEvent(int64_t Event, FUNCTION Callback, APTR *Handle) {
   return SubscribeEvent(Event, &Callback, Handle);
}

inline ERR SubscribeTimer(double Interval, FUNCTION Callback, APTR *Subscription) {
   return SubscribeTimer(Interval, &Callback, Subscription);
}

inline ERR UnsubscribeAction(OBJECTPTR Object, ACTIONID ActionID) {
   return UnsubscribeAction(Object, ActionID, nullptr);
}

// This template leverages pcObject to be the preferred entry point for any Object type or derivation.

template <pcObject T> inline ERR FreeResource(T *Object) {
   if (not Object) return ERR::NullArgs;
   return FreeResource(Object->UID);
}

template <class T> requires ((not pcComplete<T>) and (not std::is_void_v<std::remove_cv_t<T>>))
inline ERR FreeResource(T *) {
   static_assert(pcComplete<T>, "FreeResource() cannot be called with a pointer to an incomplete type.");
   return ERR::NullArgs;
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

#include <kotuku/log.h>
#include <kotuku/objects.h>

inline OBJECTID CurrentTaskID() { return ((OBJECTPTR)CurrentTask())->UID; }
inline APTR SetResourcePtr(RES Res, APTR Value) { return (APTR)(MAXINT)(SetResource(Res, (MAXINT)Value)); }

#ifdef PRV_METACLASS
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
   typedef ERR JUMP(struct lua_State *, OBJECTPTR, const struct Field *, int);

   uint32_t Hash;
   JUMP *Call;
   const struct Field *Field;

   auto operator<=>(const obj_write &Other) const {
       if (Hash < Other.Hash) return -1;
       if (Hash > Other.Hash) return 1;
       return 0;
   }

   obj_write(uint32_t pHash, JUMP pJump, const struct Field *pField) : Hash(pHash), Call(pJump), Field(pField) { }
   obj_write(uint32_t pHash, JUMP pJump) : Hash(pHash), Call(pJump) { }
   obj_write(uint32_t pHash) : Hash(pHash) { }
};

inline auto write_hash = [](const obj_write &a, const obj_write &b) { return a.Hash < b.Hash; };

typedef std::vector<obj_write> WRITE_TABLE;
#endif

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
   objMetaClass(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

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
   int     PublicSize;                  // The size of the class in bytes, as seen by the client.

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

   inline ERR getFields(std::span<const struct FieldArray> &Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      auto get_field = (ERR (*)(APTR, std::span<const struct FieldArray> &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getDictionary(std::span<struct Field> &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::span<struct Field> &))field->GetValue;
      auto error = get_field(this, Value);
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

   inline ERR getPublicSize(int &Value) noexcept {
      Value = this->PublicSize;
      return ERR::Okay;
   }

   inline ERR getActionTable(std::span<struct ActionEntry> &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto get_field = (ERR (*)(APTR, std::span<struct ActionEntry> &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getMethods(std::span<struct MethodEntry> &Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      auto get_field = (ERR (*)(APTR, std::span<struct MethodEntry> &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getLocation(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[22];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getModule(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[25];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getObjects(std::span<int> &Value) noexcept {
      auto field = &this->Class->Dictionary[26];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::span<int> &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getSubClasses(std::span<OBJECTPTR> &Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      auto get_field = (ERR (*)(APTR, std::span<OBJECTPTR> &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getRootModule(OBJECTPTR &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->GetValue(this, &Value);
   }


   // Customised field setting

   inline ERR setClassVersion(const double Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->ClassVersion = Value;
      return ERR::Okay;
   }

   inline ERR setFields(std::span<const struct FieldArray> Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(this, field, 0x00101510, &Value);
   }

   inline ERR setClassName(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->ClassName = Value;
      return ERR::Okay;
   }

   inline ERR setFileExtension(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->FileExtension = Value;
      return ERR::Okay;
   }

   inline ERR setFileDescription(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->FileDescription = Value;
      return ERR::Okay;
   }

   inline ERR setFileHeader(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->FileHeader = Value;
      return ERR::Okay;
   }

   inline ERR setPath(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Path = Value;
      return ERR::Okay;
   }

   inline ERR setIcon(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Icon = Value;
      return ERR::Okay;
   }

   inline ERR setSize(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Size = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const CLF Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setClass(const CLASSID Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->ClassID = Value;
      return ERR::Okay;
   }

   inline ERR setBaseClass(const CLASSID Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->BaseClassID = Value;
      return ERR::Okay;
   }

   inline ERR setCategory(const CCF Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Category = Value;
      return ERR::Okay;
   }

   inline ERR setPublicSize(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->PublicSize = Value;
      return ERR::Okay;
   }

   inline ERR setMethods(std::span<const struct MethodEntry> Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(this, field, 0x00101510, &Value);
   }

   inline ERR setActions(APTR Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, 0x08000400, Value);
   }

};

inline bool Object::isDerived() { return Class->ClassID != Class->BaseClassID; }
inline CLASSID Object::classID() { return Class->ClassID; }
inline CLASSID Object::baseClassID() { return Class->BaseClassID; }

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
   if (!AllocMemory(String.size()+1, MEM::STRING|MEM::NO_CLEAR, (APTR *)&newstr)) {
      memmove(newstr, String.data(), String.size());
      newstr[String.size()] = 0;
      return newstr;
   }
   else return nullptr;
}

} // pf namespace

// Function construction (refer types.h)

template <class T, class X = APTR> FUNCTION C_FUNCTION(T *pRoutine, X pMeta = 0) {
   auto func    = FUNCTION(CALL::STD_C);
   func.Context = CurrentContext();
   func.Routine = (APTR)pRoutine;
   func.Meta    = reinterpret_cast<void *>(pMeta);
   return func;
};

inline CSTRING Object::className() { return Class->ClassName.c_str(); }
