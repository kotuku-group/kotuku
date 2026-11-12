// Exercise the actual OLE adapter with a movable Unicode HDROP offer and no changes to the system clipboard.
#include "../win32/clipboard.c"
#include "../win32/drag_source.c"
#include <wchar.h>

static HWND glWindow;
static int glFailures, glAccept = 1, glDrops, glX, glY;
static char glExpectedType = DATA_FILE;
static WCHAR glPaths[700];
static const WCHAR glText[] = L"Native <text> & \x4e09\xd83d\xde00\r\nsecond line";
static char glSnapshot[1200];

static void expect(int Condition, const char *Message)
{
   if (!Condition) { fprintf(stderr, "%s\n", Message); glFailures++; }
}

typedef struct test_offer {
   IDataObject Interface;
   LONG References;
   UINT Format;
} TestOffer;

static HRESULT STDMETHODCALLTYPE offer_interface(IDataObject *Object, REFIID ID, void **Result)
{
   if (!Result) return E_POINTER;
   *Result = NULL;
   if (IsEqualGUID(ID, &IID_IUnknown) OR IsEqualGUID(ID, &IID_IDataObject)) {
      *Result = Object;
      ++((TestOffer *)Object)->References;
      return S_OK;
   }
   return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE offer_addref(IDataObject *Object)
{
   return ++((TestOffer *)Object)->References;
}

static ULONG STDMETHODCALLTYPE offer_release(IDataObject *Object)
{
   return --((TestOffer *)Object)->References;
}

static HRESULT STDMETHODCALLTYPE offer_query(IDataObject *Object, FORMATETC *Format)
{
   return Format->cfFormat IS ((TestOffer *)Object)->Format ? S_OK : DV_E_FORMATETC;
}

static HRESULT STDMETHODCALLTYPE offer_data(IDataObject *Object, FORMATETC *Format, STGMEDIUM *Medium)
{
   DROPFILES *drop;
   size_t size = sizeof(DROPFILES) + sizeof(glPaths);
   if (offer_query(Object, Format) != S_OK) return DV_E_FORMATETC;
   memset(Medium, 0, sizeof(*Medium));
   Medium->tymed = TYMED_HGLOBAL;
   if (Format->cfFormat IS CF_UNICODETEXT) {
      if (!(Medium->hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(glText)))) return E_OUTOFMEMORY;
      memcpy(GlobalLock(Medium->hGlobal), glText, sizeof(glText));
      GlobalUnlock(Medium->hGlobal);
      return S_OK;
   }
   Medium->hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
   if (!Medium->hGlobal) return E_OUTOFMEMORY;
   drop = (DROPFILES *)GlobalLock(Medium->hGlobal);
   drop->pFiles = sizeof(DROPFILES);
   drop->fWide = TRUE;
   memcpy((char *)drop + sizeof(DROPFILES), glPaths, sizeof(glPaths));
   GlobalUnlock(Medium->hGlobal);
   return S_OK;
}

static HRESULT STDMETHODCALLTYPE offer_formats(IDataObject *Object, DWORD Direction, IEnumFORMATETC **Result)
{
   FORMATETC format = { ((TestOffer *)Object)->Format, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
   if (Direction != DATADIR_GET) return E_NOTIMPL;
   return SHCreateStdEnumFmtEtc(1, &format, Result);
}

static HRESULT STDMETHODCALLTYPE offer_canonical(IDataObject *Object, FORMATETC *Input, FORMATETC *Output)
{
   Output->ptd = NULL;
   return DATA_S_SAMEFORMATETC;
}

static HRESULT STDMETHODCALLTYPE offer_set(IDataObject *Object, FORMATETC *Format, STGMEDIUM *Medium, BOOL Release)
{
   return E_NOTIMPL;
}

static IDataObjectVtbl glOfferVTable = {
   offer_interface, offer_addref, offer_release, offer_data, NULL, offer_query, offer_canonical,
   offer_set, offer_formats, NULL, NULL, NULL
};

// Optional source mode drives DoDragDrop in a separate process for the full native Flute import tests.

static DWORD glSourceStart;
static POINT glSourcePoint;

static HRESULT STDMETHODCALLTYPE source_interface(IDropSource *Object, REFIID ID, void **Result)
{
   if (!Result) return E_POINTER;
   *Result = NULL;
   if (IsEqualGUID(ID, &IID_IUnknown) OR IsEqualGUID(ID, &IID_IDropSource)) {
      *Result = Object;
      return S_OK;
   }
   return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE source_reference(IDropSource *Object) { return 1; }

static HRESULT STDMETHODCALLTYPE source_continue(IDropSource *Object, BOOL Escape, DWORD Keys)
{
   if (Escape) return DRAGDROP_S_CANCEL;
   return GetTickCount() - glSourceStart > 350 ? DRAGDROP_S_DROP : S_OK;
}

static HRESULT STDMETHODCALLTYPE source_feedback(IDropSource *Object, DWORD Effect)
{
   return DRAGDROP_S_USEDEFAULTCURSORS;
}

static volatile LONG glSourceActive;

// The press starts over the source window so that OLE's pre-drag loop sees it.  The first motion step then carries
// the cursor to the destination, where small movements keep DragOver running until QueryContinueDrag drops.

static DWORD WINAPI source_motion(void *Context)
{
   int step = 0;
   while (InterlockedCompareExchange(&glSourceActive, 1, 1)) {
      INPUT input = { 0 };
      Sleep(50);
      if (!step) SetCursorPos(glSourcePoint.x, glSourcePoint.y);
      input.type = INPUT_MOUSE;
      input.mi.dwFlags = MOUSEEVENTF_MOVE;
      input.mi.dx = step++ & 1 ? 1 : -1;
      if (GetTickCount() - glSourceStart > 400) input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
      SendInput(1, &input, sizeof(input));
   }
   return 0;
}

static int native_source(int Count, WCHAR **Args)
{
   static IDropSourceVtbl source_vtable = {
      source_interface, source_reference, source_reference, source_continue, source_feedback
   };
   IDropSource source = { &source_vtable };
   TestOffer offer = { { &glOfferVTable }, 1, CF_HDROP };
   DWORD effect = DROPEFFECT_NONE;
   POINT saved;
   HWND source_window, previous_window;
   WNDCLASSW window_class = { 0 };
   HANDLE thread;
   HRESULT result;
   int length;
   if (Count != 6) return 3;
   length = (int)wcslen(Args[4]) + 1;
   if ((length > 350) OR (wcslen(Args[5]) > 348)) return 3;
   wcscpy(glPaths, Args[4]);
   wcscpy(glPaths + length, Args[5]);
   glSourcePoint.x = (int)wcstol(Args[2], NULL, 10);
   glSourcePoint.y = (int)wcstol(Args[3], NULL, 10);
   GetCursorPos(&saved);
   winInitialiseClipboard();
   window_class.lpfnWndProc = DefWindowProcW;
   window_class.hInstance = GetModuleHandle(NULL);
   window_class.lpszClassName = L"KotukuOleSource";
   RegisterClassW(&window_class);
   previous_window = GetForegroundWindow();
   source_window = CreateWindowExW(WS_EX_TOPMOST, window_class.lpszClassName, L"OLE test source",
      WS_POPUP | WS_VISIBLE, 10, 10, 60, 60, NULL, NULL, window_class.hInstance, NULL);
   SetForegroundWindow(source_window);
   SetCapture(source_window);
   SetCursorPos(40, 40);
   glSourceStart = GetTickCount();
   {
      INPUT input = { 0 };
      input.type = INPUT_MOUSE;
      input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
      SendInput(1, &input, sizeof(input));
   }
   InterlockedExchange(&glSourceActive, 1);
   thread = CreateThread(NULL, 0, source_motion, NULL, 0, NULL);
   result = DoDragDrop(&offer.Interface, &source, DROPEFFECT_COPY, &effect);
   ReleaseCapture();
   InterlockedExchange(&glSourceActive, 0);
   WaitForSingleObject(thread, 2000);
   CloseHandle(thread);
   fprintf(stderr, "OLE result %lx, effect %lu\n", result, effect);
   SetCursorPos(saved.x, saved.y);
   DestroyWindow(source_window);
   if (IsWindow(previous_window)) SetForegroundWindow(previous_window);
   winTerminateClipboard();
   if (result != DRAGDROP_S_DROP) return 3;
   return effect IS DROPEFFECT_COPY ? 0 : 2;
}

// Optional target mode receives an export from a Kotuku process for the desktop Flute tests.  It owns a native drop
// window, drives the real cursor from a press inside the Kotuku window, and compares what it receives.
//
//    target <PressX> <PressY> drop <file:Path|text:Text>   Release over the target and expect one drop
//    target <PressX> <PressY> escape                        Press Escape over the target; no drop is expected
//    target <PressX> <PressY> return <X> <Y>                Leave, return to <X>,<Y> in Kotuku and release there
//
// In expected text, '|' represents a CRLF line break.  Exit codes: 0 success, 3 usage, 5 mismatched data,
// 6 unexpected drop, 7 missing drop, 8 the drag never reached the native target.

static int glTargetMode, glTargetDrops, glTargetMatched, glTargetHovers;
static char glTargetExpected[1024];
static volatile LONG glTargetDone;
static POINT glTargetPress, glTargetReturn, glTargetCentre;
static int glTargetAction; // 0 drop, 1 escape, 2 return

static void send_mouse(DWORD Flags)
{
   INPUT input = { 0 };
   input.type = INPUT_MOUSE;
   input.mi.dwFlags = Flags;
   SendInput(1, &input, sizeof(input));
}

static void send_key(WORD Key, DWORD Flags)
{
   INPUT input = { 0 };
   input.type = INPUT_KEYBOARD;
   input.ki.wVk = Key;
   input.ki.dwFlags = Flags;
   SendInput(1, &input, sizeof(input));
}

// Glide in small steps so that both Kotuku's capture and OLE's modal loop observe the movement.

static void glide(POINT From, POINT To, int Steps)
{
   int i;
   for (i=1; i <= Steps; i++) {
      SetCursorPos(From.x + (To.x - From.x) * i / Steps, From.y + (To.y - From.y) * i / Steps);
      send_mouse(MOUSEEVENTF_MOVE);
      Sleep(25);
   }
}

static void wiggle(POINT At, int Count)
{
   int i;
   for (i=0; i < Count; i++) {
      SetCursorPos(At.x + ((i & 1) ? 1 : -1), At.y);
      send_mouse(MOUSEEVENTF_MOVE);
      Sleep(40);
   }
}

static DWORD WINAPI target_motion(void *Context)
{
   Sleep(300);
   SetCursorPos(glTargetPress.x, glTargetPress.y);
   Sleep(150);
   send_mouse(MOUSEEVENTF_LEFTDOWN);
   Sleep(400); // The Kotuku process starts its internal drag from the press
   wiggle(glTargetPress, 4);
   glide(glTargetPress, glTargetCentre, 20);
   wiggle(glTargetCentre, 10);
   if (glTargetAction IS 1) {
      send_key(VK_ESCAPE, 0);
      send_key(VK_ESCAPE, KEYEVENTF_KEYUP);
      Sleep(200);
      send_mouse(MOUSEEVENTF_LEFTUP);
   }
   else if (glTargetAction IS 2) {
      glide(glTargetCentre, glTargetReturn, 20);
      wiggle(glTargetReturn, 10);
      send_mouse(MOUSEEVENTF_LEFTUP);
   }
   else send_mouse(MOUSEEVENTF_LEFTUP);
   Sleep(1500); // Allow the drop and the Kotuku process to complete
   InterlockedExchange(&glTargetDone, 1);
   return 0;
}

static int target_drop(int Surface, int X, int Y, const char *Types)
{
   char preference[4] = { 0, 0, 0, 0 };
   struct WinDT *data = NULL;
   int count = 0;
   glTargetDrops++;
   preference[0] = strncmp(glTargetExpected, "file:", 5) IS 0 ? DATA_FILE : DATA_TEXT;
   if ((winGetData(preference, &data, &count) IS ERR_Okay) AND (count IS 1)) {
      char *scan;
      fprintf(stderr, "Received: %s\n", (char *)data[0].Data);
      if (preference[0] IS DATA_FILE) {
         for (scan=(char *)data[0].Data; *scan; scan++) if (*scan IS '/') *scan = '\\';
         for (scan=glTargetExpected; *scan; scan++) if (*scan IS '/') *scan = '\\';
      }
      glTargetMatched = strcmp(data[0].Data, glTargetExpected + 5) IS 0;
   }
   else fprintf(stderr, "Received %d items\n", count);
   return 1;
}

static int native_target(int Count, WCHAR **Args)
{
   WNDCLASSA window_class = { 0 };
   RECT work = { 0 };
   POINT saved;
   HANDLE thread;
   MSG msg;
   int i;

   if (Count < 5) return 3;
   glTargetMode = 1;
   glTargetPress.x = (int)wcstol(Args[2], NULL, 10);
   glTargetPress.y = (int)wcstol(Args[3], NULL, 10);
   if (wcscmp(Args[4], L"drop") IS 0) {
      if (Count != 6) return 3;
      WideCharToMultiByte(CP_UTF8, 0, Args[5], -1, glTargetExpected, sizeof(glTargetExpected), NULL, NULL);
      for (i=0; glTargetExpected[i]; i++);
      { // Expand '|' to CRLF
         char expanded[1024];
         int j = 0;
         for (i=0; glTargetExpected[i] AND (j < (int)sizeof(expanded) - 2); i++) {
            if (glTargetExpected[i] IS '|') { expanded[j++] = '\r'; expanded[j++] = '\n'; }
            else expanded[j++] = glTargetExpected[i];
         }
         expanded[j] = 0;
         strcpy(glTargetExpected, expanded);
      }
   }
   else if (wcscmp(Args[4], L"escape") IS 0) glTargetAction = 1;
   else if (wcscmp(Args[4], L"return") IS 0) {
      if (Count != 7) return 3;
      glTargetAction = 2;
      glTargetReturn.x = (int)wcstol(Args[5], NULL, 10);
      glTargetReturn.y = (int)wcstol(Args[6], NULL, 10);
   }
   else return 3;

   GetCursorPos(&saved);
   winInitialiseClipboard();
   window_class.lpfnWndProc = DefWindowProcA;
   window_class.lpszClassName = "KotukuExportTarget";
   window_class.hInstance = GetModuleHandle(NULL);
   RegisterClassA(&window_class);
   SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
   glWindow = CreateWindowExA(WS_EX_TOPMOST | WS_EX_NOACTIVATE, window_class.lpszClassName, "Export target",
      WS_POPUP | WS_VISIBLE, work.right - 220, work.bottom - 220, 200, 200, NULL, NULL,
      window_class.hInstance, NULL);
   if ((!glWindow) OR (winInitDragDrop(glWindow) != ERR_Okay)) return 3;
   glTargetCentre.x = work.right - 120;
   glTargetCentre.y = work.bottom - 120;

   thread = CreateThread(NULL, 0, target_motion, NULL, 0, NULL);
   while (!InterlockedCompareExchange(&glTargetDone, 1, 1)) {
      while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
         TranslateMessage(&msg);
         DispatchMessage(&msg);
      }
      MsgWaitForMultipleObjects(0, NULL, FALSE, 20, QS_ALLINPUT);
   }
   WaitForSingleObject(thread, 2000);
   CloseHandle(thread);
   SetCursorPos(saved.x, saved.y);
   winDisableDragDrop(glWindow);
   DestroyWindow(glWindow);
   winTerminateClipboard();

   fprintf(stderr, "Target hovers: %d, drops: %d\n", glTargetHovers, glTargetDrops);
   if (!glTargetHovers) return 8;
   if (glTargetAction IS 0) {
      if (!glTargetDrops) return 7;
      if ((glTargetDrops > 1) OR (!glTargetMatched)) return 5;
      return 0;
   }
   return glTargetDrops ? 6 : 0;
}

int winResolveSurfaceID(HWND Window) { return Window IS glWindow ? 123 : 0; }
void winDragDropFromHost_Leave(void) { }
int winDragDropFromHost_Over(int Surface, int X, int Y, const char *Types, int Length)
{
   if (glTargetMode) { glTargetHovers++; return 1; }
   glX = X;
   glY = Y;
   expect(Surface IS 123, "OLE must resolve the destination window");
   expect((Length IS 1) AND (Types[0] IS glExpectedType), "Offer must advertise its numeric datatype once");
   return glAccept;
}

int winDragDropFromHost_Drop(int Surface, int X, int Y, const char *Types)
{
   char preference[4] = { DATA_FILE, 0, 0, 0 };
   struct WinDT *data = NULL;
   int count = 0;
   if (glTargetMode) return target_drop(Surface, X, Y, Types);
   glDrops++;
   expect((X IS glX) AND (Y IS glY), "Drop must use OLE's coordinate snapshot");
   if (glExpectedType IS DATA_TEXT) {
      preference[0] = DATA_TEXT;
      expect(winGetData(preference, &data, &count) IS ERR_Okay, "Text must remain readable during Drop");
      expect((count IS 1) AND (data[0].Datatype IS DATA_TEXT), "A text offer must return a single text item");
      if (count IS 1) {
         expect(strcmp(data[0].Data, "Native <text> & \xe4\xb8\x89\xf0\x9f\x98\x80\r\nsecond line") IS 0,
            "UTF-16 text, metacharacters and line endings must survive conversion to UTF-8");
         strcpy(glSnapshot, data[0].Data);
      }
      return glAccept;
   }
   expect(winGetData(preference, &data, &count) IS ERR_Okay, "The offer must remain readable during Drop");
   expect(count IS 2, "A multi-file HDROP must return both files");
   if (count IS 2) {
      expect(strcmp(data[0].Data, "C:\\space & \xe4\xb8\x89\xf0\x9f\x98\x80.txt") IS 0,
         "Unicode and supplementary characters must survive conversion to UTF-8");
      expect(strlen(data[1].Data) > MAX_PATH, "Long paths must not be truncated at MAX_PATH");
      strcpy(glSnapshot, data[0].Data);
   }
   return glAccept;
}

// Export callbacks normally implemented by the display module.

static int glExportActive = 1, glExportAccept = 1, glExportOvers, glExportDrops, glExportLeaves;
int winDragExport_Active(void) { return glExportActive; }
int winDragExport_Over(int Surface, int X, int Y)
{
   expect(Surface IS 123, "An exported drag must resolve our own destination window");
   glExportOvers++;
   return glExportAccept;
}
int winDragExport_Drop(int Surface, int X, int Y)
{
   expect(Surface IS 123, "An exported drop must resolve our own destination window");
   glExportDrops++;
   return glExportAccept;
}
void winDragExport_Leave(int X, int Y) { glExportLeaves++; }

static int read_text(IDataObject *Offer, WCHAR *Buffer, int Size)
{
   FORMATETC format = { CF_UNICODETEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
   STGMEDIUM medium;
   WCHAR *text;
   if (Offer->lpVtbl->GetData(Offer, &format, &medium) != S_OK) return 0;
   if ((text = (WCHAR *)GlobalLock(medium.hGlobal))) {
      wcsncpy(Buffer, text, Size - 1);
      Buffer[Size-1] = 0;
      text[0] = L'X'; // Recipients own their copy; the offer must not change
      GlobalUnlock(medium.hGlobal);
   }
   ReleaseStgMedium(&medium);
   return 1;
}

// Outbound offers and the drag source, without running DoDragDrop().

static void test_export(POINTL Point)
{
   const char *files[] = {
      "C:/export dir/space & \xe4\xb8\x89\xf0\x9f\x98\x80.txt", "C:/export dir/folder/", "C:/"
   };
   const char text[] = "Line <one> & \xe4\xb8\x89\nline two\r\nline three\r";
   FORMATETC format = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
   STGMEDIUM medium;
   IDataObject *offer, *files_only;
   IEnumFORMATETC *formats;
   IDropSource *source;
   WCHAR buffer[200];
   FORMATETC listed[8];
   ULONG fetched = 0;
   DWORD effect;
   int start_effect;
   POINT point = { Point.x, Point.y };

   expect(winCreateDragExport(NULL, 0, NULL, 0, 1) IS NULL, "An export without data must not create an offer");
   expect(winCreateDragExport(NULL, 0, "\xff\xfe", 2, 1) IS NULL, "Invalid UTF-8 text must not be offered");

   offer = winCreateDragExport(files, 3, text, (int)strlen(text), 77);
   expect(offer != NULL, "A file and text export must create an offer");
   if (!offer) return;

   expect(offer->lpVtbl->QueryGetData(offer, &format) IS S_OK, "Files must be offered as CF_HDROP");
   format.tymed = TYMED_ISTREAM;
   expect(offer->lpVtbl->QueryGetData(offer, &format) IS DV_E_TYMED, "Only HGLOBAL storage is offered");
   format.tymed = TYMED_HGLOBAL;
   format.cfFormat = CF_BITMAP;
   expect(offer->lpVtbl->QueryGetData(offer, &format) IS DV_E_FORMATETC, "Unsupported formats must be refused");
   expect(offer->lpVtbl->SetData(offer, &format, &medium, FALSE) IS E_NOTIMPL, "The offer must be immutable");

   if (offer->lpVtbl->EnumFormatEtc(offer, DATADIR_GET, &formats) IS S_OK) {
      formats->lpVtbl->Next(formats, 8, listed, &fetched);
      formats->lpVtbl->Release(formats);
   }
   expect(fetched IS 4, "Files, text, preferred effect and the process marker must be enumerated");

   format.cfFormat = CF_HDROP;
   if (offer->lpVtbl->GetData(offer, &format, &medium) IS S_OK) {
      HDROP drop = (HDROP)medium.hGlobal;
      expect(DragQueryFileW(drop, 0xffffffff, NULL, 0) IS 3, "Every exported path must be listed");
      DragQueryFileW(drop, 0, buffer, 200);
      expect(wcscmp(buffer, L"C:\\export dir\\space & \x4e09\xd83d\xde00.txt") IS 0,
         "Unicode paths must be converted with native separators");
      DragQueryFileW(drop, 1, buffer, 200);
      expect(wcscmp(buffer, L"C:\\export dir\\folder") IS 0, "Folders must lose their trailing separator");
      DragQueryFileW(drop, 2, buffer, 200);
      expect(wcscmp(buffer, L"C:\\") IS 0, "A drive root must keep its separator");
      ReleaseStgMedium(&medium);
   }
   else expect(0, "CF_HDROP must be readable");

   format.cfFormat = (CLIPFORMAT)fmtExportEffect;
   if (offer->lpVtbl->GetData(offer, &format, &medium) IS S_OK) {
      DWORD *value = (DWORD *)GlobalLock(medium.hGlobal);
      expect(value AND (*value IS DROPEFFECT_COPY), "The preferred effect must be copy");
      GlobalUnlock(medium.hGlobal);
      ReleaseStgMedium(&medium);
   }
   else expect(0, "The preferred drop effect must be readable");

   expect(read_text(offer, buffer, 200), "Text must be readable");
   expect(wcscmp(buffer, L"Line <one> & \x4e09\r\nline two\r\nline three\r\n") IS 0,
      "Text must be UTF-16 with CRLF line endings");
   expect(read_text(offer, buffer, 200) AND (buffer[0] IS L'L'), "Every read must return an unmodified copy");

   // Only the offer of the active export is recognised.

   expect(!winIsDragExport(offer), "An offer is not ours once its export has ended");
   glExportCookie = 78;
   expect(!winIsDragExport(offer), "An offer from another export session must not be recognised");
   glExportCookie = 77;
   expect(winIsDragExport(offer), "The active export's offer must be recognised");

   // The drop target must route our own offer to the internal session instead of the native import path.

   glDrops = 0;
   effect = DROPEFFECT_COPY;
   RKDT_DragEnter(glDropTarget, offer, 0, Point, &effect);
   expect((effect IS DROPEFFECT_COPY) AND (glExportOvers IS 1), "Our own offer must use internal feedback");
   glExportAccept = 0;
   effect = DROPEFFECT_COPY;
   RKDT_DragOver(glDropTarget, 0, Point, &effect);
   expect((effect IS DROPEFFECT_NONE) AND (glExportOvers IS 2), "Internal rejection must refuse the native drag");
   glExportAccept = 1;
   RKDT_DragLeave(glDropTarget);
   expect(glExportLeaves IS 1, "Leaving must clear the internal feedback once");
   effect = DROPEFFECT_COPY;
   RKDT_DragEnter(glDropTarget, offer, 0, Point, &effect);
   effect = DROPEFFECT_COPY;
   RKDT_Drop(glDropTarget, offer, 0, point, &effect);
   expect((effect IS DROPEFFECT_COPY) AND (glExportDrops IS 1) AND (glDrops IS 0),
      "A drop on our own window must be recorded for the internal session, not imported");
   expect(glExportLeaves IS 1, "A drop must not report a leave that would move the pointer away");

   // An offer that is no longer active, such as one from another Kotuku process, is imported normally.

   glExportCookie = 0;
   glExpectedType = DATA_FILE;
   if ((files_only = winCreateDragExport(files, 1, NULL, 0, 79))) {
      effect = DROPEFFECT_COPY;
      RKDT_DragEnter(glDropTarget, files_only, 0, Point, &effect);
      expect((effect IS DROPEFFECT_COPY) AND (glExportOvers IS 3), "Foreign Kotuku offers must use the import path");
      RKDT_DragLeave(glDropTarget);
      expect(files_only->lpVtbl->Release(files_only) IS 0, "The target must release a foreign offer");
   }
   else expect(0, "A files-only offer must be created");

   expect(offer->lpVtbl->Release(offer) IS 0, "The target must release every reference to the offer");

   // The drag source cancels on Escape or when the internal session ends, and drops on release.

   if ((source = create_export_source())) {
      expect(source->lpVtbl->QueryContinueDrag(source, TRUE, MK_LBUTTON) IS DRAGDROP_S_CANCEL, "Escape must cancel");
      expect(source->lpVtbl->QueryContinueDrag(source, FALSE, MK_LBUTTON) IS S_OK, "A held button must continue");
      expect(source->lpVtbl->QueryContinueDrag(source, FALSE, 0) IS DRAGDROP_S_DROP, "Release must drop");
      glExportActive = 0;
      expect(source->lpVtbl->QueryContinueDrag(source, FALSE, MK_LBUTTON) IS DRAGDROP_S_CANCEL,
         "An ended internal session must cancel the native drag");
      glExportActive = 1;
      expect(source->lpVtbl->GiveFeedback(source, DROPEFFECT_COPY) IS DRAGDROP_S_USEDEFAULTCURSORS,
         "Default cursors must be used");
      source->lpVtbl->Release(source);
   }
   else expect(0, "The drop source must be created");

   if (!(GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000)) {
      expect(winStartDragExport(files, 1, NULL, 0, &start_effect) IS ERR_InvalidState,
         "An export must not start without the primary button held");
   }
}

void report_windows_clip_text(void *Data) { }
void report_windows_clip_utf16(void *Data) { }
void report_windows_files(LPIDA Data, int Cut) { }
void report_windows_hdrop(LPIDA Data, int Cut, char Wide) { }

int wmain(int Count, WCHAR **Args)
{
   TestOffer offer = { { &glOfferVTable }, 1, CF_HDROP };
   WNDCLASSA window_class = { 0 };
   RECT work = { 0 };
   POINTL point;
   POINT drop_point;
   DWORD effect, sequence = GetClipboardSequenceNumber();
   char preference[4] = { DATA_FILE, 0, 0, 0 };
   struct WinDT *data;
   int count, i;
   if ((Count > 1) AND (wcscmp(Args[1], L"source") IS 0)) return native_source(Count, Args);
   if ((Count > 1) AND (wcscmp(Args[1], L"target") IS 0)) return native_target(Count, Args);
   wcscpy(glPaths, L"C:\\space & \x4e09\xd83d\xde00.txt");
   {
      WCHAR *second = glPaths + wcslen(glPaths) + 1;
      wcscpy(second, L"C:\\");
      for (i=3; i < 350; i++) second[i] = L'x';
      wcscpy(second + 350, L".txt");
   }
   winInitialiseClipboard();
   window_class.lpfnWndProc = DefWindowProcA;
   window_class.lpszClassName = "KotukuDropTest";
   window_class.hInstance = GetModuleHandle(NULL);
   RegisterClassA(&window_class);
   SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
   glWindow = CreateWindowExA(WS_EX_TOPMOST | WS_EX_NOACTIVATE, window_class.lpszClassName, "Drop test",
      WS_POPUP | WS_VISIBLE, work.right - 120, work.bottom - 120, 100, 100, NULL, NULL,
      window_class.hInstance, NULL);
   expect(glWindow != NULL, "Native drop target window must be created");
   expect(winInitDragDrop(glWindow) IS ERR_Okay, "OLE drop target must register");
   if (!glWindow OR !glDropTarget) return 1;
   point.x = work.right - 100;
   point.y = work.bottom - 100;
   drop_point.x = point.x;
   drop_point.y = point.y;

   effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
   RKDT_DragEnter(glDropTarget, &offer.Interface, 0, point, &effect);
   expect((effect IS DROPEFFECT_COPY) AND (offer.References IS 2), "Enter must retain a copy offer");
   effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
   RKDT_DragOver(glDropTarget, MK_SHIFT, point, &effect);
   expect(effect IS DROPEFFECT_NONE, "Shift/move must be refused");
   effect = DROPEFFECT_COPY;
   RKDT_DragOver(glDropTarget, MK_CONTROL, point, &effect);
   expect(effect IS DROPEFFECT_COPY, "Ctrl must select copy");
   effect = DROPEFFECT_MOVE;
   RKDT_DragOver(glDropTarget, 0, point, &effect);
   expect(effect IS DROPEFFECT_NONE, "A move-only source must not be advertised as copy");
   glAccept = 0;
   effect = DROPEFFECT_COPY;
   RKDT_DragOver(glDropTarget, 0, point, &effect);
   expect(effect IS DROPEFFECT_NONE, "A rejected destination must return the rejected-drop cursor");
   glAccept = 1;
   effect = DROPEFFECT_COPY;
   RKDT_Drop(glDropTarget, &offer.Interface, 0, drop_point, &effect);
   expect((effect IS DROPEFFECT_COPY) AND (glDrops IS 1), "An accepted drop must report copy once");
   expect(offer.References IS 1, "Drop must release the native offer");
   expect(winGetData(preference, &data, &count) IS ERR_NoData, "Deferred reads must fail after Drop");
   expect(glSnapshot[0] IS 'C', "The receiver's copied paths must survive the offer");

   effect = DROPEFFECT_COPY;
   RKDT_DragEnter(glDropTarget, &offer.Interface, 0, point, &effect);
   RKDT_DragLeave(glDropTarget);
   expect((offer.References IS 1) AND (glDrops IS 1), "Leaving must release without delivering a drop");
   offer.Format = CF_BITMAP;
   effect = DROPEFFECT_COPY;
   RKDT_DragEnter(glDropTarget, &offer.Interface, 0, point, &effect);
   expect(effect IS DROPEFFECT_NONE, "Unsupported formats must not advertise acceptance");
   RKDT_DragLeave(glDropTarget);

   offer.Format = CF_UNICODETEXT;
   glExpectedType = DATA_TEXT;
   glSnapshot[0] = 0;
   effect = DROPEFFECT_COPY;
   RKDT_DragEnter(glDropTarget, &offer.Interface, 0, point, &effect);
   expect(effect IS DROPEFFECT_COPY, "Text offers must be accepted as copies");
   effect = DROPEFFECT_COPY;
   RKDT_Drop(glDropTarget, &offer.Interface, 0, drop_point, &effect);
   expect((effect IS DROPEFFECT_COPY) AND (glDrops IS 2), "An accepted text drop must report copy once");
   expect(offer.References IS 1, "Text drops must release the native offer");
   preference[0] = DATA_TEXT;
   expect(winGetData(preference, &data, &count) IS ERR_NoData, "Deferred text reads must fail after Drop");
   expect(glSnapshot[0] IS 'N', "The receiver's copied text must survive the offer");

   test_export(point);
   winDisableDragDrop(glWindow);
   DestroyWindow(glWindow);
   winTerminateClipboard();
   expect(sequence IS GetClipboardSequenceNumber(), "Native dragging must not publish to the normal clipboard");
   return glFailures ? 1 : 0;
}
