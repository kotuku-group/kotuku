#pragma once

struct WinDT {
   int Datatype;
   int Length;
   void *Data;
};

#ifdef __cplusplus
extern "C" {
#endif

extern int glIgnoreClip;
extern int glClipboardUpdates;
extern int8_t glOleInit;

int winResolveSurfaceID(HWND Window);
int winClipboardChanged(void);
void winDisableDragDrop(HWND Window);
void winCreateScreenClassClipboard(void);
void winInitialiseClipboard(void);
int winDragDropFromHost_Over(int, int, int, const char *, int);
int winDragDropFromHost_Drop(int, int, int, const char *);
void winDragDropFromHost_Leave(void);

// Outbound dragging.  The display module implements the winDragExport_ callbacks; drag_source.c owns the OLE objects.

int winDragExport_Active(void);
int winDragExport_Over(int SurfaceID, int X, int Y);
int winDragExport_Drop(int SurfaceID, int X, int Y);
void winDragExport_Leave(int X, int Y);
int winStartDragExport(const char *const *Files, int FileCount, const char *Text, int TextLength, int *Effect);
#ifdef __objidl_h__
int winIsDragExport(IDataObject *Data);
IDataObject * winCreateDragExport(const char *const *Files, int FileCount, const char *Text, int TextLength,
   DWORD Cookie);
#endif

void win_clipboard_updated(void);
void winTerminateClipboard(void);

#ifdef __cplusplus
}

namespace display {

extern "C" int winAddClip(int, const void *, int, int);
extern "C" int winAddFileClip(const char16_t *, int, int);
extern "C" void winClearClipboard(void);
extern "C" void winCopyClipboard(void);
extern "C" int winExtractFile(void *, int, char *, int);
extern "C" void winGetClip(int);
extern "C" int winInitDragDrop(HWND);
extern "C" int winCurrentClipboardID(void);
extern "C" int winGetData(char *, struct WinDT **, int *);

} // namespace display
#endif
