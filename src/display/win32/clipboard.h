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
void winDragDropFromHost_Drop(int, char *);
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
