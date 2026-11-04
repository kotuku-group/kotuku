// Exercise the actual OLE adapter with a movable Unicode HDROP offer and no changes to the system clipboard.
#include "../win32/clipboard.c"
#include <wchar.h>

static HWND glWindow;
static int glFailures, glAccept = 1, glDrops, glX, glY;
static WCHAR glPaths[700];
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

int winResolveSurfaceID(HWND Window) { return Window IS glWindow ? 123 : 0; }
void winDragDropFromHost_Leave(void) { }
int winDragDropFromHost_Over(int Surface, int X, int Y, const char *Types, int Length)
{
   glX = X;
   glY = Y;
   expect(Surface IS 123, "OLE must resolve the destination window");
   expect((Length IS 1) AND (Types[0] IS DATA_FILE), "Offer must advertise numeric file data once");
   return glAccept;
}

int winDragDropFromHost_Drop(int Surface, int X, int Y, const char *Types)
{
   char preference[4] = { DATA_FILE, 0, 0, 0 };
   struct WinDT *data = NULL;
   int count = 0;
   glDrops++;
   expect((X IS glX) AND (Y IS glY), "Drop must use OLE's coordinate snapshot");
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
   winDisableDragDrop(glWindow);
   DestroyWindow(glWindow);
   winTerminateClipboard();
   expect(sequence IS GetClipboardSequenceNumber(), "Native dragging must not publish to the normal clipboard");
   return glFailures ? 1 : 0;
}
