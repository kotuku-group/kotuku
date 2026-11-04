
#define _WIN32_WINNT 0x0600 // Allow Windows Vista function calls
#define WINVER 0x0600

#ifdef _MSC_VER
#pragma warning (disable : 4244 4311 4312 4267 4244 4068) // Disable annoying VC++ typecast warnings
#endif

#include <kotuku/system/errors_c.h>

#include "../drivers/win32/keys.h"
#include <windows.h>
#include <windowsx.h>
//#include <resource.h>
#include <winuser.h>
#include <shlobj.h>
#include <objidl.h>

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>

#include "clipboard.h"

enum {
   CT_DATA=0,
   CT_AUDIO,
   CT_IMAGE,
   CT_FILE,
   CT_OBJECT,
   CT_TEXT,
   CT_END      // End
};

#define OR  ||
#define AND &&
#define IS  ==

//#define MSG(...) fprintf(stderr, __VA_ARGS__)
#define MSG(...)

#define CLIP_DATA    (1<<CT_DATA)   // 1
#define CLIP_AUDIO   (1<<CT_AUDIO)  // 2
#define CLIP_IMAGE   (1<<CT_IMAGE)  // 4
#define CLIP_FILE    (1<<CT_FILE)   // 8
#define CLIP_OBJECT  (1<<CT_OBJECT) // 16
#define CLIP_TEXT    (1<<CT_TEXT)   // 32

enum { // From core.h
   DATA_TEXT=1,    // Standard ascii text
   DATA_RAW,       // Raw unprocessed data
   DATA_DEVICE_INPUT, // Device activity
   DATA_XML,       // Markup based text data.  NOTE: For clipboard data, the top-level encapsulating tag must declare the type of XML, e.g. <html>, <ripple>.  For plain XML, use <xml>
   DATA_AUDIO,     // Audio file data, recognised by the Sound class
   DATA_RECORD,    // Database record
   DATA_IMAGE,     // Image file data, recognised by the Image class
   DATA_REQUEST,   // Make a request for item data
   DATA_RECEIPT,   // Receipt for item data, in response to an earlier request
   DATA_FILE,      // File location (the data will reflect the complete file path)
   DATA_CONTENT    // Document content (between XML tags) - sent by document objects only
};

#define HIDA_GetPIDLFolder(pida) (LPCITEMIDLIST)(((LPBYTE)pida)+(pida)->aoffset[0])
#define HIDA_GetPIDLItem(pida, i) (LPCITEMIDLIST)(((LPBYTE)pida)+(pida)->aoffset[i+1])

typedef struct rkDropTarget {
   IDropTarget idt;
   LONG lRefCount;

   struct WinDT *DataItems;
   int    TotalItems;
	char   *tb_pDragFile;
   IDataObject *CurrentDataObject;
   char Datatypes[8];
   int DatatypeCount;
   void *ItemData;
} RK_IDROPTARGET;

typedef struct rkDropTargetVTable
{
   BEGIN_INTERFACE
      HRESULT (STDMETHODCALLTYPE *QueryInterface)(struct rkDropTarget *pThis, REFIID riid, void  **ppvObject);
      ULONG (STDMETHODCALLTYPE *AddRef)(struct rkDropTarget *pThis);
      ULONG (STDMETHODCALLTYPE *Release)(struct rkDropTarget *pThis);
      HRESULT (STDMETHODCALLTYPE *DragEnter)(struct rkDropTarget *pThis, IDataObject *pDataObject, DWORD dwKeyState, POINTL pt, DWORD *pdwEffect);
      HRESULT (STDMETHODCALLTYPE *DragOver)(struct rkDropTarget *pThis, DWORD dwKeyState, POINTL pt, DWORD *pdwEffect);
      HRESULT (STDMETHODCALLTYPE *DragLeave)(struct rkDropTarget *pThis);
      HRESULT (STDMETHODCALLTYPE *Drop)(struct rkDropTarget *pThis, IDataObject *pDataObject, DWORD dwKeyState, POINT pt, DWORD *pdwEffect);
   END_INTERFACE
} RK_IDROPTARGET_VTBL;

static void clear_data(struct rkDropTarget *Self);

static HANDLE glHeap = NULL;
static UINT fmtShellIDList;

static ULONG   STDMETHODCALLTYPE RKDT_AddRef(struct rkDropTarget *Self);
static HRESULT STDMETHODCALLTYPE RKDT_Drop(struct rkDropTarget *Self, IDataObject *Data, DWORD grfKeyState, POINT pt, DWORD * pdwEffect);
static int     STDMETHODCALLTYPE RKDT_AssessDatatype(struct rkDropTarget *Self, IDataObject *Data, char *Result, int Length);
static HRESULT STDMETHODCALLTYPE RKDT_DragEnter(struct rkDropTarget *Self, IDataObject *Data, DWORD grfKeyState, POINTL pt, DWORD *pdwEffect);

void report_windows_clip_text(void *);
void report_windows_clip_utf16(void *);
void report_windows_files(LPIDA, int);
void report_windows_hdrop(LPIDA, int, char);

void winCopyClipboard(void);

int8_t glOleInit = 0;
int glIgnoreClip = 0;
int glClipboardUpdates = 0;
static UINT fmtShellIDList = 0;
static UINT fmtPasteSucceeded = 0;
static UINT fmtPerformedDropEffect = 0;
static UINT fmtPreferredDropEffect = 0;
static UINT fmtKotukuClip = 0;

void winCreateScreenClassClipboard(void)
{
   if (!fmtShellIDList) fmtShellIDList = RegisterClipboardFormat(CFSTR_SHELLIDLIST);
   if (!fmtPasteSucceeded) fmtPasteSucceeded = RegisterClipboardFormat(CFSTR_PASTESUCCEEDED);
   if (!fmtPerformedDropEffect) fmtPerformedDropEffect = RegisterClipboardFormat(CFSTR_PERFORMEDDROPEFFECT);
   if (!fmtPreferredDropEffect) fmtPreferredDropEffect = RegisterClipboardFormat(CFSTR_PREFERREDDROPEFFECT);
   if (!fmtKotukuClip) fmtKotukuClip = RegisterClipboardFormat("Kotuku");
}

void winInitialiseClipboard(void)
{
   winCreateScreenClassClipboard();
   if (!glOleInit) {
      HRESULT result = OleInitialize(NULL);
      if (SUCCEEDED(result)) glOleInit = 1;
      else glOleInit = 2;
   }
}

int winClipboardChanged(void)
{
   glClipboardUpdates++;
   return GetTickCount() - glIgnoreClip >= 2000;
}

void winDisableDragDrop(HWND Window)
{
   RevokeDragDrop(Window);
   winDragDropFromHost_Leave();
}

//********************************************************************************************************************
// Convert the windows datatypes to Kotuku datatypes.

static int STDMETHODCALLTYPE RKDT_AssessDatatype(struct rkDropTarget *Self, IDataObject *Data, char *Result, int Length)
{
   // Advertise only formats that get_data() can actually retrieve.  QueryGetData also avoids duplicate
   // text formats consuming all slots in the datatype list.
   const UINT formats[] = { CF_HDROP, CF_UNICODETEXT, CF_TEXT, CF_TIFF, CF_RIFF };
   const char types[] = { DATA_FILE, DATA_TEXT, DATA_TEXT, DATA_IMAGE, DATA_AUDIO };
   int count = 0, i, j;
   (void)Self;
   for (i=0; i < sizeof(formats) / sizeof(formats[0]); i++) {
      FORMATETC format = { formats[i], NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
      if (Data->lpVtbl->QueryGetData(Data, &format) != S_OK) continue;
      for (j=0; j < count; j++) if (Result[j] IS types[i]) break;
      if (j < count) continue;
      if (count >= Length-1) break;
      Result[count++] = types[i];
   }
   Result[count] = 0;
   return count;
}

//********************************************************************************************************************
// Get a pointer to our interface

static HRESULT STDMETHODCALLTYPE RKDT_QueryInterface(struct rkDropTarget *Self, REFIID iid, void ** ppvObject)
{
   MSG("rkDropTarget::QueryInterface()\n");

	if (ppvObject IS NULL) return E_POINTER;

	// Supports IID_IUnknown and IID_IDropTarget
	if (IsEqualGUID(iid, &IID_IUnknown) OR IsEqualGUID(iid, &IID_IDropTarget)) {
		*ppvObject = (void *)Self;
		RKDT_AddRef(Self);
		return S_OK;
	}

	return E_NOINTERFACE;
}

//********************************************************************************************************************

static ULONG STDMETHODCALLTYPE RKDT_AddRef(struct rkDropTarget *Self)
{
   MSG("rkDropTarget::AddRef()\n");
   return InterlockedIncrement(&Self->lRefCount);
}

//********************************************************************************************************************

static ULONG STDMETHODCALLTYPE RKDT_Release(struct rkDropTarget *Self)
{
   MSG("rkDropTarget::Release()\n");

   LONG nCount;
   if ((nCount = InterlockedDecrement(&Self->lRefCount)) == 0) {
      clear_data(Self);
      if (Self->CurrentDataObject) Self->CurrentDataObject->lpVtbl->Release(Self->CurrentDataObject);
      if (glHeap) HeapFree(glHeap, 0, Self);
   }

	return nCount;
}

//********************************************************************************************************************
// The drag action continues

static void release_offer(struct rkDropTarget *Self)
{
   IDataObject *data = Self->CurrentDataObject;
   Self->CurrentDataObject = NULL;
   Self->DatatypeCount = 0;
   memset(Self->Datatypes, 0, sizeof(Self->Datatypes));
   if (data) data->lpVtbl->Release(data);
   winDragDropFromHost_Leave();
}

static DWORD assess_drop(struct rkDropTarget *Self, DWORD Keys, POINTL Point, DWORD Effects)
{
   POINT point = { Point.x, Point.y };
   HWND window = WindowFromPoint(point);
   int surface = winResolveSurfaceID(window);
   RECT client;
   POINT local = point;
   if ((!surface) OR (!Self->DatatypeCount) OR (!(Effects & DROPEFFECT_COPY)) OR
       (Keys & MK_SHIFT) OR (Keys & MK_ALT) OR
       (!ScreenToClient(window, &local)) OR (!GetClientRect(window, &client)) OR (!PtInRect(&client, local))) {
      winDragDropFromHost_Leave();
      return DROPEFFECT_NONE;
   }
   return winDragDropFromHost_Over(surface, point.x, point.y, Self->Datatypes, Self->DatatypeCount) ?
      DROPEFFECT_COPY : DROPEFFECT_NONE;
}

static HRESULT STDMETHODCALLTYPE RKDT_DragOver(struct rkDropTarget *Self, DWORD Keys, POINTL Point, DWORD *Effect)
{
   if (!Effect) return E_POINTER;
   *Effect = assess_drop(Self, Keys, Point, *Effect);
   return S_OK;
}

static HRESULT STDMETHODCALLTYPE RKDT_DragLeave(struct rkDropTarget *Self)
{
   release_offer(Self);
   return S_OK;
}

static HRESULT STDMETHODCALLTYPE RKDT_DragEnter(struct rkDropTarget *Self, IDataObject *Data, DWORD Keys,
   POINTL Point, DWORD *Effect)
{
   if ((!Data) OR (!Effect)) return E_INVALIDARG;
   release_offer(Self);
   Data->lpVtbl->AddRef(Data);
   Self->CurrentDataObject = Data;
   Self->DatatypeCount = RKDT_AssessDatatype(Self, Data, Self->Datatypes, sizeof(Self->Datatypes));
   *Effect = assess_drop(Self, Keys, Point, *Effect);
   return S_OK;
}

static HRESULT STDMETHODCALLTYPE RKDT_Drop(struct rkDropTarget *Self, IDataObject *Data, DWORD Keys,
   POINT Point, DWORD *Effect)
{
   POINTL point = { Point.x, Point.y };
   DWORD allowed;
   int surface;
   if ((!Data) OR (!Effect)) return E_INVALIDARG;
   allowed = *Effect;
   *Effect = DROPEFFECT_NONE;
   if (Data != Self->CurrentDataObject) {
      release_offer(Self);
      Data->lpVtbl->AddRef(Data);
      Self->CurrentDataObject = Data;
   }
   Self->DatatypeCount = RKDT_AssessDatatype(Self, Data, Self->Datatypes, sizeof(Self->Datatypes));
   if (assess_drop(Self, Keys, point, allowed) IS DROPEFFECT_COPY) {
      POINT location = { Point.x, Point.y };
      surface = winResolveSurfaceID(WindowFromPoint(location));
      if (winDragDropFromHost_Drop(surface, Point.x, Point.y, Self->Datatypes)) *Effect = DROPEFFECT_COPY;
   }
   // All receivers must snapshot data synchronously.  Only the copied path list may outlive this callback.
   release_offer(Self);
   return S_OK;
}

//********************************************************************************************************************

static void clear_data(struct rkDropTarget *Self)
{
   if (Self->ItemData) {
      free(Self->ItemData);
      Self->ItemData = NULL;
   }

   if (Self->DataItems) {
      int i;
      for (i=0; i < Self->TotalItems; i++) free(Self->DataItems[i].Data);
      Self->TotalItems = 0;
      free(Self->DataItems);
      Self->DataItems = NULL;
   }
}

//********************************************************************************************************************

static int lock_hglobal_data(IDataObject *Data, UINT Format, STGMEDIUM *Medium, void **OutData, int *Found)
{
   FORMATETC fmt = { Format, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };

   *OutData = NULL;
   *Found = 0;

   if (!(Data->lpVtbl->GetData(Data, &fmt, Medium) IS S_OK)) return ERR_Failed;

   *Found = 1;
   if (!(*OutData = GlobalLock(Medium->hGlobal))) {
      ReleaseStgMedium(Medium);
      return ERR_Lock;
   }

   return ERR_Okay;
}

//********************************************************************************************************************

static void unlock_hglobal_data(STGMEDIUM *Medium)
{
   GlobalUnlock(Medium->hGlobal);
   ReleaseStgMedium(Medium);
}

//********************************************************************************************************************

static int publish_single_data(struct rkDropTarget *Self, int Datatype, int Length, struct WinDT **OutData,
   int *OutTotal)
{
   if ((Self->DataItems = (struct WinDT *)malloc(sizeof(struct WinDT)))) {
      Self->DataItems[0].Datatype = Datatype;
      Self->DataItems[0].Length   = Length;
      Self->DataItems[0].Data     = Self->ItemData;
      *OutData  = Self->DataItems;
      *OutTotal = 1;
      return ERR_Okay;
   }
   else return ERR_AllocMemory;
}

//********************************************************************************************************************

static int copy_hglobal_data(struct rkDropTarget *Self, IDataObject *Data, UINT Format, int Datatype,
   struct WinDT **OutData, int *OutTotal, int *Found)
{
   STGMEDIUM stgm;
   void *raw;
   int error, size;

   error = lock_hglobal_data(Data, Format, &stgm, &raw, Found);
   if (!*Found) return ERR_Failed;
   if (!(error IS ERR_Okay)) return error;

   size = GlobalSize(stgm.hGlobal);
   if ((Self->ItemData = malloc(size))) {
      memcpy(Self->ItemData, raw, size);
      error = publish_single_data(Self, Datatype, size, OutData, OutTotal);
   }
   else error = ERR_AllocMemory;

   unlock_hglobal_data(&stgm);
   return error;
}

//********************************************************************************************************************

static int get_data(struct rkDropTarget *Self, char *Preference, struct WinDT **OutData, int *OutTotal)
{
   IDataObject *data;
   int p, error;

   MSG("rkDropTarget::GetData()\n");

   if ((!Preference) OR (!OutData) OR (!OutTotal)) return ERR_NullArgs;

   clear_data(Self);

   *OutData = NULL;
   *OutTotal = 0;
   data = Self->CurrentDataObject;
   if (!data) return ERR_NoData;
   for (p=0; p < 4; p++) { // Up to 4 data preferences may be specified
      switch (Preference[p]) {
         case DATA_TEXT:
         case DATA_XML: {
            STGMEDIUM stgm;
            void *raw_data;
            int size, found;

            error = lock_hglobal_data(data, CF_UNICODETEXT, &stgm, &raw_data, &found);
            if (found) {
               const WCHAR *text = (const WCHAR *)raw_data;
               size_t length = 0, limit;
               if (!(error IS ERR_Okay)) return error;
               limit = GlobalSize(stgm.hGlobal) / sizeof(WCHAR);
               while ((length < limit) AND text[length]) length++;
               if (length IS limit) error = ERR_InvalidData;
               else {
                  size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, (int)length + 1,
                     NULL, 0, NULL, NULL);
                  if (!size) error = ERR_InvalidData;
                  else if ((Self->ItemData = malloc(size))) {
                     WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, (int)length + 1,
                        Self->ItemData, size, NULL, NULL);
                     error = publish_single_data(Self, DATA_TEXT, size, OutData, OutTotal);
                  }
                  else error = ERR_AllocMemory;
               }
               unlock_hglobal_data(&stgm);
               return error;
            }

            error = copy_hglobal_data(Self, data, CF_TEXT, DATA_TEXT, OutData, OutTotal, &found);
            if (found) return error;

            break;
         }

         case DATA_IMAGE: {
            int found;

            error = copy_hglobal_data(Self, data, CF_TIFF, DATA_IMAGE, OutData, OutTotal, &found);
            if (found) return error;

            break;
         }

         case DATA_AUDIO: {
            int found;

            error = copy_hglobal_data(Self, data, CF_RIFF, DATA_AUDIO, OutData, OutTotal, &found);
            if (found) return error;

            break;
         }

         case DATA_FILE: {
            STGMEDIUM stgm;
            void *raw_data;
            int i, total, size, found;

            error = lock_hglobal_data(data, CF_HDROP, &stgm, &raw_data, &found);
            if (found) {
               HDROP drop;
               if (!(error IS ERR_Okay)) return error;
               // DragQueryFile expects the HGLOBAL handle, not the address returned by GlobalLock.
               drop = (HDROP)stgm.hGlobal;
               total = DragQueryFileW(drop, 0xffffffff, NULL, 0);
               Self->DataItems = (struct WinDT *)calloc(total, sizeof(struct WinDT));
               error = total ? ERR_Okay : ERR_NoData;
               if ((total) AND (!Self->DataItems)) error = ERR_AllocMemory;
               for (i=0; (i < total) AND (error IS ERR_Okay); i++) {
                  UINT length = DragQueryFileW(drop, i, NULL, 0);
                  WCHAR *path = (WCHAR *)malloc((length + 2) * sizeof(WCHAR));
                  DWORD attrib;
                  char *utf8;
                  if (!path) { error = ERR_AllocMemory; break; }
                  if (DragQueryFileW(drop, i, path, length + 1) != length) {
                     free(path);
                     error = ERR_Failed;
                     break;
                  }
                  // The shell names folders without a trailing separator; Kotuku file APIs require one.
                  attrib = GetFileAttributesW(path);
                  if ((attrib != INVALID_FILE_ATTRIBUTES) AND (attrib & FILE_ATTRIBUTE_DIRECTORY) AND (length) AND
                      (path[length-1] != L'\\') AND (path[length-1] != L'/')) {
                     path[length] = L'\\';
                     path[length+1] = 0;
                  }
                  size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, NULL, 0, NULL, NULL);
                  utf8 = size ? (char *)malloc(size) : NULL;
                  if (utf8) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, utf8, size, NULL, NULL);
                  free(path);
                  if (!utf8) { error = ERR_AllocMemory; break; }
                  Self->DataItems[i].Datatype = DATA_FILE;
                  Self->DataItems[i].Length = size;
                  Self->DataItems[i].Data = utf8;
                  Self->TotalItems = i + 1;
               }
               if (error IS ERR_Okay) {
                  *OutData = Self->DataItems;
                  *OutTotal = total;
               }
               unlock_hglobal_data(&stgm);
               return error;
            }

            error = lock_hglobal_data(data, fmtShellIDList, &stgm, &raw_data, &found);
            if (found) {
               LPIDA pida;
               TCHAR path[MAX_PATH], folderpath[MAX_PATH];
               LPCITEMIDLIST item, folder;
               int pos, j, index, folderlen, size;

               MSG("ShellIDList discovered.\n");

               if (!(error IS ERR_Okay)) return error;

               error = ERR_Okay;
               pida = (LPIDA)raw_data;
               folder = HIDA_GetPIDLFolder(pida);
               if (SHGetPathFromIDList(folder, folderpath)) {
                  // Calculate the size of the file strings: (FolderLength * Total) + (Lengths of each filename)

                  for (folderlen=0; folderpath[folderlen]; folderlen++);
                  size = folderlen * pida->cidl;

                  // Add lengths of each file name

                  for (index=0; index < (int)pida->cidl; index++) {
                     item = HIDA_GetPIDLItem(pida, index);
                     if (SHGetPathFromIDList(item, path)) {
                        for (j=0; path[j]; j++);
                        while ((j > 0) AND (path[j-1] != '/') AND (path[j-1] != '\\')) j--;
                        for (i=0; path[j+i]; i++);
                        size += i + 1;
                     }
                     else {
                        error = ERR_Failed;
                        break;
                     }
                  }

                  if (!error) {
                     if ((Self->ItemData = malloc(size)) AND
                         (Self->DataItems = (struct WinDT *)malloc(sizeof(struct WinDT) * pida->cidl))) {
                        char *str = (char *)Self->ItemData;
                        pos = 0;
                        for (index=0; index < (int)pida->cidl; index++) {
                           item = HIDA_GetPIDLItem(pida, index);
                           if (SHGetPathFromIDList(item, path)) {
                              Self->DataItems[index].Datatype = DATA_FILE;
                              Self->DataItems[index].Data     = str + pos;

                              // Copy the root folder path first

                              for (j=0; folderpath[j]; j++) str[pos++] = folderpath[j];

                              // Go to the end of the string and then find the start of the filename

                              for (j=0; path[j]; j++);
                              while ((j > 0) AND (path[j-1] != '/') AND (path[j-1] != '\\')) j--;

                              while (path[j]) str[pos++] = path[j++];
                              str[pos++] = 0;

                              Self->DataItems[index].Length = (int)(((char *)str + pos) -
                                 (char *)Self->DataItems[index].Data);
                           }
                           else {
                              error = ERR_Failed;
                              break;
                           }
                        }

                        if (!error) {
                           *OutData  = Self->DataItems;
                           *OutTotal = pida->cidl;
                        }
                     }
                     else error = ERR_AllocMemory;
                  }
               }
               else error = ERR_Failed;

               unlock_hglobal_data(&stgm);
               return error;
            }
            break;
         }
      }
   }

#if 0
   // Other means of data access
	WCHAR *strFileName;
   switch (stgMedium.tymed) {
      case TYMED_FILE:
         strFileName = stgMedium.lpszFileName;
         //...
         CoTaskMemFree(stgMedium.lpszFileName);
         break;

      case TYMED_ISTREAM:
         tb_pDragFile = new COleStreamFile(stgMedium.pstm);
         DWORD siz = tb_pDragFile->GetLength();
         LPTSTR buffer = Data.GetBufferSetLength((int)siz);
         tb_pDragFile->Read(buffer, (UINT)siz); // typesafe here because GetLength >=0
         Data.ReleaseBuffer();
         break;
   }
#endif

   return ERR_Failed;
}

//********************************************************************************************************************

static RK_IDROPTARGET *glDropTarget = NULL;

int winInitDragDrop(HWND Window)
{
   MSG("Initialising drag and drop.\n");

   fmtShellIDList = RegisterClipboardFormat(CFSTR_SHELLIDLIST);

   glHeap = GetProcessHeap();

   if (!glDropTarget) {
      static RK_IDROPTARGET_VTBL idt_vtbl = {
         RKDT_QueryInterface,
         RKDT_AddRef,
         RKDT_Release,
         RKDT_DragEnter,
         RKDT_DragOver,
         RKDT_DragLeave,
         RKDT_Drop
      };

      if (!(glDropTarget = HeapAlloc(glHeap, HEAP_ZERO_MEMORY, sizeof(RK_IDROPTARGET)))) return ERR_Failed;
      glDropTarget->idt.lpVtbl   = (void *)&idt_vtbl;
      glDropTarget->lRefCount    = 1;
      glDropTarget->tb_pDragFile = NULL;
   }

   if (RegisterDragDrop(Window, &glDropTarget->idt) != S_OK) {
      MSG("RegisterDragDrop() failed.\n");
      return ERR_Failed;
   }

   return ERR_Okay;
}

//********************************************************************************************************************

int winGetData(char *Preference, struct WinDT **OutData, int *OutTotal)
{
   if ((!Preference) OR (!OutData) OR (!OutTotal)) return ERR_NullArgs;
   if (!glDropTarget) return ERR_Failed;
   return get_data(glDropTarget, Preference, OutData, OutTotal);
}

//********************************************************************************************************************

void winClearClipboard(void)
{
   if (OpenClipboard(NULL)) {
      EmptyClipboard();
      CloseClipboard();
   }
}

//********************************************************************************************************************
// Called from clipAddText() etc

int winAddClip(int Datatype, const void *Data, int Size, int Cut)
{
   MSG("winAddClip()\n");

   UINT format;
   switch(Datatype) {
      case CLIP_DATA:   return ERR_NoSupport; break;
      case CLIP_AUDIO:  format = CF_WAVE; break;
      case CLIP_IMAGE:  format = CF_BITMAP; break;
      case CLIP_OBJECT: return ERR_NoSupport; break;
      case CLIP_TEXT:   format = CF_UNICODETEXT; break;
      default:          return ERR_NoSupport;
   }

   if (OpenClipboard(NULL)) {
      int error;

      EmptyClipboard();

      HGLOBAL hdata;
      if ((hdata = GlobalAlloc(GMEM_FIXED, Size))) {
         char * pdata;
         if ((pdata = (char *)GlobalLock(hdata))) {
            memcpy(pdata, Data, Size);
            GlobalUnlock(hdata);

            glIgnoreClip = GetTickCount();

            if (SetClipboardData(format, hdata) == NULL) {
               GlobalFree(hdata);
               error = ERR_Failed;
            }
            else error = ERR_Okay;
         }
         else {
            GlobalFree(hdata);
            error = ERR_Lock;
         }
      }
      else error = ERR_AllocMemory;

      CloseClipboard();
      return error;
   }
   else return ERR_Failed;
}

//********************************************************************************************************************

int winAddFileClip(const unsigned short *Path, int Size, int Cut)
{
   MSG("winAddFileClip()\n");

   if (OpenClipboard(NULL)) {
      int error;

      HGLOBAL hdata;
      if ((hdata = GlobalAlloc(GMEM_MOVEABLE, Size + sizeof(DROPFILES)))) {
         char * pdata;
         if ((pdata = (char *)GlobalLock(hdata))) {
            DROPFILES *df = (DROPFILES*)pdata;
            df->pFiles = sizeof(DROPFILES);
            df->pt.x = 0;
            df->pt.y = 0;
            df->fNC = 0;
            df->fWide = 1;
            memcpy(pdata+sizeof(DROPFILES), Path, Size);
            GlobalUnlock(hdata);

            EmptyClipboard();
            glIgnoreClip = GetTickCount();

            if (SetClipboardData(CF_HDROP, hdata) == NULL) {
               GlobalFree(hdata);
               error = ERR_Failed;
            }
            else error = ERR_Okay;
         }
         else {
            GlobalFree(hdata);
            error = ERR_Lock;
         }
      }
      else error = ERR_AllocMemory;

      CloseClipboard();
      return error;
   }
   else return ERR_Failed;
}

//********************************************************************************************************************

void winGetClip(int Datatype)
{
   UINT format;

   switch (Datatype) {
      case CLIP_DATA:   return; break;
      case CLIP_AUDIO:  format = CF_WAVE; break;
      case CLIP_IMAGE:  format = CF_BITMAP; break;
      case CLIP_FILE:   format = CF_HDROP; break;
      case CLIP_OBJECT: return; break;
      case CLIP_TEXT:   format = CF_UNICODETEXT; break;
      default:
         return;
   }

   GetClipboardData(CF_UNICODETEXT);
}

//********************************************************************************************************************
// The clipboard ID increments every time that a new item appears on the Windows clipboard.

int winCurrentClipboardID(void)
{
   return glClipboardUpdates;
}

//********************************************************************************************************************

void winCopyClipboard(void)
{
   void *pdata;
   IDataObject *pDataObj;
   IEnumFORMATETC *pEnumFmt;
   FORMATETC fmt;

   if (!glOleInit) {
      MSG("OLE not initialised.\n");
      return;
   }

   MSG("winCopyClipboard()\n");

   glIgnoreClip = GetTickCount(); // Needed to avoid automated successive calls to this function.

   HRESULT result; // Other apps can block the clipboard, so we need to be able to reattempt access.
	for (int attempt=0; attempt < 8; attempt++) {
      result = OleGetClipboard(&pDataObj);
      if (result IS S_OK) break;
      Sleep(1);
	}

   if (result IS S_OK) {
      // Enumerate the formats supported by this clip.  It is assumed that the formats
      // that are encountered first have priority.

      if (pDataObj->lpVtbl->EnumFormatEtc(pDataObj, DATADIR_GET, &pEnumFmt) IS S_OK) {
         while (pEnumFmt->lpVtbl->Next(pEnumFmt, 1, &fmt, NULL) IS S_OK) {
            if (fmt.cfFormat IS CF_UNICODETEXT) {
               FORMATETC fmt = { CF_UNICODETEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
               STGMEDIUM stgm;
               if (pDataObj->lpVtbl->GetData(pDataObj, &fmt, &stgm) IS S_OK) {
                  if ((pdata = GlobalLock(stgm.hGlobal))) {
                     report_windows_clip_utf16(pdata);
                     GlobalUnlock(stgm.hGlobal);
                  }
                  ReleaseStgMedium(&stgm);
               }
               break;
            }
            else if ((fmt.cfFormat IS CF_TEXT) OR (fmt.cfFormat IS CF_OEMTEXT) OR (fmt.cfFormat IS CF_DSPTEXT)) {
               FORMATETC fmt = { CF_TEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
               STGMEDIUM stgm;
               if (pDataObj->lpVtbl->GetData(pDataObj, &fmt, &stgm) IS S_OK) {
                  if ((pdata = GlobalLock(stgm.hGlobal))) {
                     report_windows_clip_utf16(pdata);
                     GlobalUnlock(stgm.hGlobal);
                  }
                  ReleaseStgMedium(&stgm);
               }
               break;
            }
            else if (fmt.cfFormat IS CF_HDROP) {
               FORMATETC fmt = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
               STGMEDIUM stgm, effect;
               DWORD *effect_data;
               char cut_operation = 0;

               if (pDataObj->lpVtbl->GetData(pDataObj, &fmt, &stgm) IS S_OK) {
                  FORMATETC fmt = { fmtPreferredDropEffect, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
                  if (pDataObj->lpVtbl->GetData(pDataObj, &fmt, &effect) IS S_OK) {
                     if ((effect_data = (DWORD *)GlobalLock(effect.hGlobal))) {
                        if (*effect_data IS DROPEFFECT_MOVE) cut_operation = 1;
                        GlobalUnlock(effect.hGlobal);
                     }
                     ReleaseStgMedium(&effect);
                  }

                  DROPFILES *df;
                  if ((df = (DROPFILES *)GlobalLock(stgm.hGlobal))) {
                     report_windows_hdrop((LPIDA)(((const char *)df) + (ptrdiff_t)df->pFiles), cut_operation, df->fWide);
                     GlobalUnlock(stgm.hGlobal);
                  }
                  ReleaseStgMedium(&stgm);
               }
               break;
            }
            else if (fmt.cfFormat IS fmtShellIDList) {
               // List of files found
               FORMATETC fmt = { fmtShellIDList, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
               STGMEDIUM stgm, effect;
               LPIDA pida;
               DWORD *effect_data;
               char cut_operation = 0;

               if (pDataObj->lpVtbl->GetData(pDataObj, &fmt, &stgm) IS S_OK) {
                  FORMATETC fmt = { fmtPreferredDropEffect, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
                  if (pDataObj->lpVtbl->GetData(pDataObj, &fmt, &effect) IS S_OK) {
                     if ((effect_data = (DWORD *)GlobalLock(effect.hGlobal))) {
                        if (*effect_data IS DROPEFFECT_MOVE) cut_operation = 1;
                        GlobalUnlock(effect.hGlobal);
                     }
                     ReleaseStgMedium(&effect);
                  }

                  if ((pida = (LPIDA)GlobalLock(stgm.hGlobal))) {
                     report_windows_files(pida, cut_operation);
                     GlobalUnlock(stgm.hGlobal);
                  }
                  ReleaseStgMedium(&stgm);
                }
                break;
            }
         }
         pEnumFmt->lpVtbl->Release(pEnumFmt);
      }
      else MSG("EnumFormatEtc() failed.\n");

      pDataObj->lpVtbl->Release(pDataObj);
   }
   else MSG("OleGetClipboard() failed.\n");
}

//********************************************************************************************************************

int winExtractFile(LPIDA pida, int Index, char *Result, int Size)
{
   TCHAR path[MAX_PATH];

   if (Index >= (int)pida->cidl) return 0;

   LPCITEMIDLIST list = HIDA_GetPIDLFolder(pida);
   if (SHGetPathFromIDList(list, path)) {
      int pos, j;
      for (pos=0; (path[pos]) AND (pos < Size-1); pos++) Result[pos] = path[pos];
      if ((Result[pos-1] != '\\') AND (pos < Size-1)) Result[pos++] = '\\';

      list = HIDA_GetPIDLItem(pida, Index);
      if (SHGetPathFromIDList(list, path)) {
         for (j=0; path[j]; j++);
         while ((j > 0) AND (path[j-1] != '/') AND (path[j-1] != '\\')) j--;

         while ((path[j]) AND (pos < Size-1)) Result[pos++] = path[j++];
         Result[pos] = 0;

         return 1;
      }
      else return 0;
   }
   else return 0;
}

//********************************************************************************************************************

void winTerminateClipboard(void)
{
   if (glDropTarget) {
      RKDT_Release(glDropTarget);
      glDropTarget = NULL;
   }
   if (glOleInit == 1) OleUninitialize();
   glOleInit = 0;
}
