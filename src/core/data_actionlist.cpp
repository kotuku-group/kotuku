
#include <kotuku/main.h>
#include <kotuku/strings.hpp>

#define FDEF static const struct FunctionField

FDEF argsClipboard[]     = { { "Mode", FD_INT }, { 0, 0 } };
FDEF argsCopyData[]      = { { "Dest", FD_OBJECTPTR  }, { 0, 0 } };
FDEF argsDataFeed[]      = { { "Object", FD_OBJECTPTR }, { "Datatype", FD_INT }, { "Buffer", FD_PTR }, { "Size", FD_INT|FD_PTRSIZE }, { 0, 0 } };
FDEF argsDragDrop[]      = { { "Source", FD_OBJECTPTR }, { "Item", FD_INT }, { "Datatype", FDF_CPPSTRING }, { 0, 0 } };
FDEF argsDraw[]          = { { "X", FD_INT }, { "Y", FD_INT }, { "Width", FD_INT }, { "Height", FD_INT }, { 0, 0 } };
FDEF argsGetKey[]        = { { "Field", FDF_CPPSTRING }, { "Value", FDF_CPPSTRING|FD_RESULT|FD_MUTABLE }, { 0, 0 } };
FDEF argsMove[]          = { { "DeltaX", FD_DOUBLE }, { "DeltaY", FD_DOUBLE }, { "DeltaZ", FD_DOUBLE }, { 0, 0 } };
FDEF argsMoveToPoint[]   = { { "X", FD_DOUBLE }, { "Y", FD_DOUBLE }, { "Z", FD_DOUBLE }, { "Flags", FD_INT }, { 0, 0 } };
FDEF argsNewChild[]      = { { "NewChild", FD_OBJECTPTR }, { 0, 0 } };
FDEF argsNewOwner[]      = { { "NewOwner", FD_OBJECTPTR }, { 0, 0 } };
FDEF argsRead[]          = { { "Buffer", FD_PTRBUFFER|FD_MUTABLE }, { "Length", FD_INT|FD_BUFSIZE }, { "Result", FD_INT|FD_RESULT }, { 0, 0 } };
FDEF argsRedimension[]   = { { "X", FD_DOUBLE }, { "Y", FD_DOUBLE }, { "Z", FD_DOUBLE }, { "Width", FD_DOUBLE }, { "Height", FD_DOUBLE }, { "Depth", FD_DOUBLE }, { 0, 0 } };
FDEF argsRedo[]          = { { "Steps", FD_INT }, { 0, 0 } };
FDEF argsRename[]        = { { "Name", FDF_CPPSTRING }, { 0, 0 } };
FDEF argsResize[]        = { { "Width", FD_DOUBLE }, { "Height", FD_DOUBLE }, { "Depth", FD_DOUBLE }, { 0, 0 } };
FDEF argsSaveImage[]     = { { "Dest", FD_OBJECTPTR }, { "Class", FD_INT }, { 0, 0 } };
FDEF argsSaveToObject[]  = { { "Dest", FD_OBJECTPTR }, { "Class", FD_INT }, { 0, 0 } };
FDEF argsSeek[]          = { { "Offset", FD_DOUBLE }, { "Position", FD_INT }, { 0, 0 } };
FDEF argsSetKey[]        = { { "Field", FDF_CPPSTRING }, { "Value", FDF_CPPSTRING }, { 0, 0 } };
FDEF argsUndo[]          = { { "Steps", FD_INT }, { 0, 0 } };
FDEF argsWrite[]         = { { "Buffer", FD_PTR|FD_BUFFER }, { "Length", FD_INT|FD_BUFSIZE }, { "Result", FD_INT|FD_RESULT }, { 0, 0 } };

extern "C" const struct ActionTable ActionTable[] = { // Sorted by action ID.
   { 0, 0, 0, 0 },
   { kt::strhash("SIGNAL"),         0, "Signal", 0 },
   { kt::strhash("ACTIVATE"),       0, "Activate", 0 },
   { kt::strhash("REDIMENSION"),    sizeof(struct acRedimension), "Redimension", argsRedimension },
   { kt::strhash("CLEAR"),          0, "Clear", 0 },
   { kt::strhash("FREEWARNING"),    0, "FreeWarning", 0 },
   { kt::strhash("ENABLE"),         0, "Enable", 0 },
   { kt::strhash("COPYDATA"),       sizeof(struct acCopyData), "CopyData", argsCopyData },
   { kt::strhash("DATAFEED"),       sizeof(struct acDataFeed), "DataFeed", argsDataFeed },
   { kt::strhash("DEACTIVATE"),     0, "Deactivate", 0 },
   { kt::strhash("DRAW"),           sizeof(struct acDraw), "Draw", argsDraw },
   { kt::strhash("FLUSH"),          0, "Flush", 0 },
   { kt::strhash("FOCUS"),          0, "Focus", 0 },
   { kt::strhash("FREE"),           0, "Free", 0 },
   { kt::strhash("SAVESETTINGS"),   0, "SaveSettings", 0 },
   { kt::strhash("GETKEY"),         sizeof(struct acGetKey), "GetKey", argsGetKey },
   { kt::strhash("DRAGDROP"),       sizeof(struct acDragDrop), "DragDrop", argsDragDrop },
   { kt::strhash("HIDE"),           0, "Hide", 0 },
   { kt::strhash("INIT"),           0, "Init", 0 },
   { kt::strhash("LOCK"),           0, "Lock", 0 },
   { kt::strhash("LOSTFOCUS"),      0, "LostFocus", 0 },
   { kt::strhash("MOVE"),           sizeof(struct acMove), "Move", argsMove },
   { kt::strhash("MOVETOBACK"),     0, "MoveToBack", 0 },
   { kt::strhash("MOVETOFRONT"),    0, "MoveToFront", 0 },
   { kt::strhash("NEWCHILD"),       sizeof(struct acNewChild), "NewChild", argsNewChild },
   { kt::strhash("NEWOWNER"),       sizeof(struct acNewOwner), "NewOwner", argsNewOwner },
   { kt::strhash("NEWOBJECT"),      0, "NewObject", 0 },
   { kt::strhash("REDO"),           sizeof(struct acRedo), "Redo", argsRedo },
   { kt::strhash("QUERY"),          0, "Query", 0 },
   { kt::strhash("READ"),           sizeof(struct acRead), "Read", argsRead },
   { kt::strhash("RENAME"),         sizeof(struct acRename), "Rename", argsRename },
   { kt::strhash("RESET"),          0, "Reset", 0 },
   { kt::strhash("RESIZE"),         sizeof(struct acResize), "Resize", argsResize },
   { kt::strhash("SAVEIMAGE"),      sizeof(struct acSaveImage), "SaveImage", argsSaveImage },
   { kt::strhash("SAVETOOBJECT"),   sizeof(struct acSaveToObject), "SaveToObject", argsSaveToObject },
   { kt::strhash("MOVETOPOINT"),    sizeof(struct acMoveToPoint), "MoveToPoint", argsMoveToPoint },
   { kt::strhash("SEEK"),           sizeof(struct acSeek), "Seek", argsSeek },
   { kt::strhash("SETKEY"),         sizeof(struct acSetKey), "SetKey", argsSetKey },
   { kt::strhash("SHOW"),           0, "Show", 0 },
   { kt::strhash("UNDO"),           sizeof(struct acUndo), "Undo", argsUndo },
   { kt::strhash("UNLOCK"),         0, "Unlock", 0 },
   { kt::strhash("NEXT"),           0, "Next", 0 },
   { kt::strhash("PREV"),           0, "Prev", 0 },
   { kt::strhash("WRITE"),          sizeof(struct acWrite), "Write", argsWrite },
   { kt::strhash("SETFIELD"),       0, "SetField", 0 }, // Used for logging SetField() calls
   { kt::strhash("CLIPBOARD"),      sizeof(struct acClipboard), "Clipboard", argsClipboard },
   { kt::strhash("REFRESH"),        0, "Refresh", 0 },
   { kt::strhash("DISABLE"),        0, "Disable", 0 },
   { kt::strhash("NEWPLACEMENT"),   0, "NewPlacement", 0 },
   { kt::strhash("FREEPLACEMENT"),  0, "FreePlacement", 0 },
   { 0, 0, 0, 0 }
};
