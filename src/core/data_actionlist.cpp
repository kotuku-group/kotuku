
#include <kotuku/main.h>
#include <kotuku/strings.hpp>

#define FDEF static const struct FunctionField

FDEF argsClipboard[]     = { { "Mode", FD_INT }, { 0, 0 } };
FDEF argsCopyData[]      = { { "Dest", FD_OBJECTPTR  }, { 0, 0 } };
FDEF argsDataFeed[]      = { { "Object", FD_OBJECTPTR }, { "Datatype", FD_INT }, { "Buffer", FDF_SPAN|FD_BYTE }, { 0, 0 } };
FDEF argsDragDrop[]      = { { "Source", FD_OBJECTPTR }, { "Item", FD_INT }, { "Datatype", FDF_CPPSTRING }, { 0, 0 } };
FDEF argsDraw[]          = { { "X", FD_INT }, { "Y", FD_INT }, { "Width", FD_INT }, { "Height", FD_INT }, { 0, 0 } };
FDEF argsGetKey[]        = { { "Field", FDF_CPPSTRING }, { "Value", FDF_CPPSTRING|FD_RESULT|FD_MUTABLE }, { 0, 0 } };
FDEF argsMove[]          = { { "DeltaX", FD_DOUBLE }, { "DeltaY", FD_DOUBLE }, { "DeltaZ", FD_DOUBLE }, { 0, 0 } };
FDEF argsMoveToPoint[]   = { { "X", FD_DOUBLE }, { "Y", FD_DOUBLE }, { "Z", FD_DOUBLE }, { "Flags", FD_INT }, { 0, 0 } };
FDEF argsNewChild[]      = { { "NewChild", FD_OBJECTPTR }, { 0, 0 } };
FDEF argsNewOwner[]      = { { "NewOwner", FD_OBJECTPTR }, { 0, 0 } };
FDEF argsRead[]          = { { "Buffer", FDF_SPAN|FD_MUTABLE|FD_BYTE }, { "Result", FD_INT|FD_RESULT }, { 0, 0 } };
FDEF argsRedimension[]   = { { "X", FD_DOUBLE }, { "Y", FD_DOUBLE }, { "Z", FD_DOUBLE }, { "Width", FD_DOUBLE }, { "Height", FD_DOUBLE }, { "Depth", FD_DOUBLE }, { 0, 0 } };
FDEF argsRedo[]          = { { "Steps", FD_INT }, { 0, 0 } };
FDEF argsRename[]        = { { "Name", FDF_CPPSTRING }, { 0, 0 } };
FDEF argsResize[]        = { { "Width", FD_DOUBLE }, { "Height", FD_DOUBLE }, { "Depth", FD_DOUBLE }, { 0, 0 } };
FDEF argsSaveImage[]     = { { "Dest", FD_OBJECTPTR }, { "Class", FD_INT }, { 0, 0 } };
FDEF argsSaveToObject[]  = { { "Dest", FD_OBJECTPTR }, { "Class", FD_INT }, { 0, 0 } };
FDEF argsSeek[]          = { { "Offset", FD_DOUBLE }, { "Position", FD_INT }, { 0, 0 } };
FDEF argsSetKey[]        = { { "Field", FDF_CPPSTRING }, { "Value", FDF_CPPSTRING }, { 0, 0 } };
FDEF argsUndo[]          = { { "Steps", FD_INT }, { 0, 0 } };
FDEF argsWrite[]         = { { "Buffer", FDF_SPAN|FD_BYTE }, { "Result", FD_INT|FD_RESULT }, { 0, 0 } };

extern "C" const struct ActionTable ActionTable[] = { // Sorted by action ID.
   { 0, 0, 0, 0 },
   { kt::strhash("signal"),         0, "Signal", 0 },
   { kt::strhash("activate"),       0, "Activate", 0 },
   { kt::strhash("redimension"),    sizeof(struct acRedimension), "Redimension", argsRedimension },
   { kt::strhash("clear"),          0, "Clear", 0 },
   { kt::strhash("freewarning"),    0, "FreeWarning", 0 },
   { kt::strhash("enable"),         0, "Enable", 0 },
   { kt::strhash("copydata"),       sizeof(struct acCopyData), "CopyData", argsCopyData },
   { kt::strhash("datafeed"),       sizeof(struct acDataFeed), "DataFeed", argsDataFeed },
   { kt::strhash("deactivate"),     0, "Deactivate", 0 },
   { kt::strhash("draw"),           sizeof(struct acDraw), "Draw", argsDraw },
   { kt::strhash("flush"),          0, "Flush", 0 },
   { kt::strhash("focus"),          0, "Focus", 0 },
   { kt::strhash("free"),           0, "Free", 0 },
   { kt::strhash("savesettings"),   0, "SaveSettings", 0 },
   { kt::strhash("getkey"),         sizeof(struct acGetKey), "GetKey", argsGetKey },
   { kt::strhash("dragdrop"),       sizeof(struct acDragDrop), "DragDrop", argsDragDrop },
   { kt::strhash("hide"),           0, "Hide", 0 },
   { kt::strhash("init"),           0, "Init", 0 },
   { kt::strhash("lock"),           0, "Lock", 0 },
   { kt::strhash("lostfocus"),      0, "LostFocus", 0 },
   { kt::strhash("move"),           sizeof(struct acMove), "Move", argsMove },
   { kt::strhash("movetoback"),     0, "MoveToBack", 0 },
   { kt::strhash("movetofront"),    0, "MoveToFront", 0 },
   { kt::strhash("newchild"),       sizeof(struct acNewChild), "NewChild", argsNewChild },
   { kt::strhash("newowner"),       sizeof(struct acNewOwner), "NewOwner", argsNewOwner },
   { kt::strhash("new"),            0, "New", 0 },
   { kt::strhash("redo"),           sizeof(struct acRedo), "Redo", argsRedo },
   { kt::strhash("query"),          0, "Query", 0 },
   { kt::strhash("read"),           sizeof(struct acRead), "Read", argsRead },
   { kt::strhash("rename"),         sizeof(struct acRename), "Rename", argsRename },
   { kt::strhash("reset"),          0, "Reset", 0 },
   { kt::strhash("resize"),         sizeof(struct acResize), "Resize", argsResize },
   { kt::strhash("saveimage"),      sizeof(struct acSaveImage), "SaveImage", argsSaveImage },
   { kt::strhash("savetoobject"),   sizeof(struct acSaveToObject), "SaveToObject", argsSaveToObject },
   { kt::strhash("movetopoint"),    sizeof(struct acMoveToPoint), "MoveToPoint", argsMoveToPoint },
   { kt::strhash("seek"),           sizeof(struct acSeek), "Seek", argsSeek },
   { kt::strhash("setkey"),         sizeof(struct acSetKey), "SetKey", argsSetKey },
   { kt::strhash("show"),           0, "Show", 0 },
   { kt::strhash("undo"),           sizeof(struct acUndo), "Undo", argsUndo },
   { kt::strhash("unlock"),         0, "Unlock", 0 },
   { kt::strhash("next"),           0, "Next", 0 },
   { kt::strhash("prev"),           0, "Prev", 0 },
   { kt::strhash("write"),          sizeof(struct acWrite), "Write", argsWrite },
   { kt::strhash("setfield"),       0, "SetField", 0 }, // Used for logging SetField() calls
   { kt::strhash("clipboard"),      sizeof(struct acClipboard), "Clipboard", argsClipboard },
   { kt::strhash("refresh"),        0, "Refresh", 0 },
   { kt::strhash("disable"),        0, "Disable", 0 },
   { 0, 0, 0, 0 }
};
