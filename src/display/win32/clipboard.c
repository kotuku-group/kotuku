
#define _WIN32_WINNT 0x0600 // Allow Windows Vista function calls
#define WINVER 0x0600

#ifdef _MSC_VER
#pragma warning (disable : 4244 4311 4312 4267 4244 4068) // Disable annoying VC++ typecast warnings
#endif

#include <kotuku/system/errors_c.h>

#include "keys.h"
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

#include "windows.h"

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

//********************************************************************************************************************
// Convert the windows datatypes to Kotuku datatypes.

static int STDMETHODCALLTYPE RKDT_AssessDatatype(struct rkDropTarget *Self, IDataObject *Data, char *Result, int Length)
{
   IEnumFORMATETC *eformat;
   FORMATETC fmt;
   int i;
   char dt;

   i = 0;
   if (Data->lpVtbl->EnumFormatEtc(Data, DATADIR_GET, &eformat) IS S_OK) {
      while (eformat->lpVtbl->Next(eformat, 1, &fmt, NULL) IS S_OK) {
         TCHAR szBuf[100];
         if (GetClipboardFormatName(fmt.cfFormat, szBuf, sizeof(szBuf))) {
            MSG("Format name: %d '%s'\n", fmt.cfFormat, szBuf);
         }
         else MSG("Format: %d\n", fmt.cfFormat);

         switch (fmt.cfFormat) {
            case CF_TEXT:
            case CF_UNICODETEXT:
            case CF_OEMTEXT:
               dt = DATA_TEXT; break;

            case CF_HDROP:
               dt = DATA_FILE; break;

            case CF_BITMAP:
            case CF_DIB:
            case CF_METAFILEPICT:
            case CF_TIFF:
               dt = DATA_IMAGE; break;

            case CF_RIFF:
            case CF_WAVE:
               dt = DATA_AUDIO; break;

            //case CF_HTML:
            //   dt = DATA_XML; break;

            default:
               dt = 0; break;
         }

         if (dt) Result[i++] = dt;
         if (i >= Length-1) break;
      }
      eformat->lpVtbl->Release(eformat);
   }

   Result[i] = 0;
   return i;
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
      if (Self->ItemData) {
         free(Self->ItemData);
         Self->ItemData = NULL;
      }

      if (Self->DataItems) {
         free(Self->DataItems);
         Self->DataItems = NULL;
      }
      if (glHeap) HeapFree(glHeap, 0, Self);
   }

	return nCount;
}

//********************************************************************************************************************
// The drag action continues

static HRESULT STDMETHODCALLTYPE RKDT_DragOver(struct rkDropTarget *Self, DWORD grfKeyState, POINTL pt, DWORD * pdwEffect)
{
   MSG("rkDropTarget::DragOver()\n");
	*pdwEffect = DROPEFFECT_COPY;
	return S_OK;
}

//********************************************************************************************************************
// The drag action leaves your window - no dropping

static HRESULT STDMETHODCALLTYPE RKDT_DragLeave(struct rkDropTarget *Self)
{
   MSG("rkDropTarget::DragLeave()\n");
	return S_OK;
}

//********************************************************************************************************************
// The drag action enters your window - get the item

static HRESULT STDMETHODCALLTYPE RKDT_DragEnter(struct rkDropTarget *Self, IDataObject *Data, DWORD grfKeyState, POINTL pt, DWORD *pdwEffect)
{
   MSG("rkDropTarget::DragEnter %dx%d\n", (int)pt.x, (int)pt.y);

   //HWND window = WindowFromPoint(pt);

   // Use DROPEFFECT_NONE if the datatype isn't supported, otherwise use DROPEFFECT_COPY.

	*pdwEffect = DROPEFFECT_COPY;
	return S_OK;
}

//********************************************************************************************************************
// The data have been dropped here, so process it

static HRESULT STDMETHODCALLTYPE RKDT_Drop(struct rkDropTarget *Self, IDataObject *Data, DWORD grfKeyState, POINT pt, DWORD * pdwEffect)
{
   HWND window;
   int surface_id, total;
   char datatypes[10];

   MSG("rkDropTarget::Drop(%dx%d)\n", (int)pt.x, (int)pt.y);

	*pdwEffect = DROPEFFECT_NONE;

   window = WindowFromPoint(pt);
   surface_id = winLookupSurfaceID(window);
   if (!surface_id) {
      MSG("rkDropTarget::Drop() Unable to determine surface ID from window, aborting.\n");
      return S_OK;
   }

   if (!(total = RKDT_AssessDatatype(Self, Data, datatypes, sizeof(datatypes)))) {
      MSG("rkDropTarget::Drop() Datatype not recognised or supported.\n");
      return S_OK;
   }

   // Calling winDragDropFromHost() will send an AC::DragDrop to the underlying surface.  If an object accepts the data,
   // it will send a DATA_REQUEST to the Display that represents the surface.  At this point we can copy the
   // clipboard from the host and send it to the client.  This entire process will occur within this call, so long as
   // all the calls are direct and the messaging system isn't used.  Otherwise the data will be lost as Windows
   // cannot be expected to hold onto the data after this method returns.

   Self->CurrentDataObject = Data;
   winDragDropFromHost_Drop(surface_id, datatypes);
   Self->CurrentDataObject = NULL;

	*pdwEffect = DROPEFFECT_COPY;

   return S_OK;
}

//********************************************************************************************************************

static int get_data(struct rkDropTarget *Self, char *Preference, struct WinDT **OutData, int *OutTotal)
{
	IDataObject *data;
   STGMEDIUM stgm;
	FORMATETC fmt = { CF_TEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
   int p, error;

   MSG("rkDropTarget::GetData()\n");

   if ((!Preference) OR (!OutData) OR (!OutTotal)) return ERR_NullArgs;

   if (Self->ItemData) {
      free(Self->ItemData);
      Self->ItemData = NULL;
   }

   if (Self->DataItems) {
      free(Self->DataItems);
      Self->DataItems = NULL;
   }

   data = Self->CurrentDataObject;
   for (p=0; p < 4; p++) { // Up to 4 data preferences may be specified
      switch (Preference[p]) {
         case DATA_TEXT:
         case DATA_XML: {
            int chars, u8len, i, size;
            unsigned short value;
            unsigned char *u8str;
            unsigned short *wstr;

            fmt.cfFormat = CF_UNICODETEXT;
	         if (data->lpVtbl->GetData(data, &fmt, &stgm) IS S_OK) {
               if ((wstr = (unsigned short *)GlobalLock(stgm.hGlobal))) {
                  u8len = 0;
                  for (chars=0; wstr[chars]; chars++) {
                     if (wstr[chars] < 128) u8len++;
                     else if (wstr[chars] < 0x800) u8len += 2;
                     else u8len += 3;
                  }

                  if ((Self->ItemData = malloc(u8len+1))) {
                     u8str = (unsigned char *)Self->ItemData;
                     i = 0;
                     for (chars=0; wstr[chars]; chars++) {
                        value = wstr[chars];
                        if (value < 128) {
                           u8str[i++] = (unsigned char)value;
                        }
                        else if (value < 0x800) {
                           u8str[i+1] = (value & 0x3f) | 0x80;
                           value  = value>>6;
                           u8str[i] = value | 0xc0;
                           i += 2;
                        }
                        else {
                           u8str[i+2] = (value & 0x3f)|0x80;
                           value  = value>>6;
                           u8str[i+1] = (value & 0x3f)|0x80;
                           value  = value>>6;
                           u8str[i] = value | 0xe0;
                           i += 3;
                        }
                     }
                     u8str[i++] = 0;

                     if ((Self->DataItems = (struct WinDT *)malloc(sizeof(struct WinDT)))) {
                        Self->DataItems[0].Datatype = DATA_TEXT;
                        Self->DataItems[0].Length = i;
                        Self->DataItems[0].Data = Self->ItemData;
                        *OutData  = Self->DataItems;
                        *OutTotal = 1;
                        error = ERR_Okay;
                     }
                     else error = ERR_AllocMemory;
                  }
                  else error = ERR_AllocMemory;

                  GlobalUnlock(stgm.hGlobal);
               }
               else error = ERR_Lock;

               ReleaseStgMedium(&stgm);
               return error;
            }

            fmt.cfFormat = CF_TEXT;
	         if (data->lpVtbl->GetData(data, &fmt, &stgm) IS S_OK) {
               unsigned char *str;
               if ((str = (unsigned char *)GlobalLock(stgm.hGlobal))) {
                  size = GlobalSize(stgm.hGlobal);
                  if ((Self->ItemData = malloc(size))) {
                     memcpy(Self->ItemData, str, size);

                     if ((Self->DataItems = (struct WinDT *)malloc(sizeof(struct WinDT)))) {
                        Self->DataItems[0].Datatype = DATA_TEXT;
                        Self->DataItems[0].Length   = size;
                        Self->DataItems[0].Data     = Self->ItemData;
                        *OutData  = Self->DataItems;
                        *OutTotal = 1;
                        error = ERR_Okay;
                     }
                     else error = ERR_AllocMemory;
                  }
                  else error = ERR_AllocMemory;
                  GlobalUnlock(stgm.hGlobal);
               }
               else error = ERR_Lock;

               ReleaseStgMedium(&stgm);
               return error;
            }

            break;
         }

         case DATA_IMAGE: {
            int size;

            fmt.cfFormat = CF_TIFF;
	         if (data->lpVtbl->GetData(data, &fmt, &stgm) IS S_OK) {
               unsigned char *raw;
               if ((raw = (unsigned char *)GlobalLock(stgm.hGlobal))) {
                  size = GlobalSize(stgm.hGlobal);
                  if ((Self->ItemData = malloc(size))) {
                     memcpy(Self->ItemData, raw, size);

                     if ((Self->DataItems = (struct WinDT *)malloc(sizeof(struct WinDT)))) {
                        Self->DataItems[0].Datatype = DATA_IMAGE;
                        Self->DataItems[0].Length   = size;
                        Self->DataItems[0].Data     = Self->ItemData;
                        *OutData  = Self->DataItems;
                        *OutTotal = 1;
                        error = ERR_Okay;
                     }
                     else error = ERR_AllocMemory;
                  }
                  else error = ERR_AllocMemory;
                  GlobalUnlock(stgm.hGlobal);
               }
               else error = ERR_Lock;

               ReleaseStgMedium(&stgm);
               return error;
            }

            break;
         }

         case DATA_AUDIO: {
            int size;

            fmt.cfFormat = CF_RIFF;
	         if (data->lpVtbl->GetData(data, &fmt, &stgm) IS S_OK) {
               unsigned char *raw;
               if ((raw = (unsigned char *)GlobalLock(stgm.hGlobal))) {
                  size = GlobalSize(stgm.hGlobal);
                  if ((Self->ItemData = malloc(size))) {
                     memcpy(Self->ItemData, raw, size);

                     if ((Self->DataItems = (struct WinDT *)malloc(sizeof(struct WinDT)))) {
                        Self->DataItems[0].Datatype = DATA_AUDIO;
                        Self->DataItems[0].Length   = size;
                        Self->DataItems[0].Data     = Self->ItemData;
                        *OutData  = Self->DataItems;
                        *OutTotal = 1;
                        error = ERR_Okay;
                     }
                     else error = ERR_AllocMemory;
                  }
                  else error = ERR_AllocMemory;
                  GlobalUnlock(stgm.hGlobal);
               }
               else error = ERR_Lock;

               ReleaseStgMedium(&stgm);
               return error;
            }

            break;
         }

         case DATA_FILE: {
            HDROP raw;
            int i, total, item, len, size;
            TCHAR path[MAX_PATH];
            char *str;

            fmt.cfFormat = CF_HDROP;
	         if (data->lpVtbl->GetData(data, &fmt, &stgm) IS S_OK) {
               if ((raw = (HDROP)GlobalLock(stgm.hGlobal))) {
                  total = DragQueryFile(raw, 0xffffffff, NULL, 0);

                  size = 0;
                  for (i=0; i < total; i++) {
                     size += DragQueryFile(raw, i, path, sizeof(path)) + 1;
                  }

                  if (!size);
                  else if ((Self->ItemData = malloc(size))) {
                     if ((Self->DataItems = (struct WinDT *)malloc(sizeof(struct WinDT) * total))) {
                        str = (char *)Self->ItemData;
                        for (item=0; item < total; item++) {
                           len = DragQueryFile(raw, item, str, MAX_PATH) + 1;

                           Self->DataItems[item].Datatype = DATA_FILE;
                           Self->DataItems[item].Length   = len;
                           Self->DataItems[item].Data     = str;
                           str += len;
                        }
                        *OutData  = Self->DataItems;
                        *OutTotal = total;
                        error = ERR_Okay;
                     }
                     else error = ERR_AllocMemory;
                  }
                  else error = ERR_AllocMemory;
                  GlobalUnlock(stgm.hGlobal);
               }
               else error = ERR_Lock;

               ReleaseStgMedium(&stgm);
               return error;
            }

            fmt.cfFormat = fmtShellIDList;
            if (data->lpVtbl->GetData(data, &fmt, &stgm) IS S_OK) {
               LPIDA pida;
               TCHAR path[MAX_PATH], folderpath[MAX_PATH];
               LPCITEMIDLIST item, folder;
               int pos, j, index, folderlen;

               MSG("ShellIDList discovered.\n");

               error = ERR_Okay;

               if ((pida = (LPIDA)GlobalLock(stgm.hGlobal))) {
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
                        if ((Self->ItemData = malloc(size)) AND (Self->DataItems = (struct WinDT *)malloc(sizeof(struct WinDT) * pida->cidl))) {
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

                                 Self->DataItems[index].Length = (int)(((char *)str + pos) - (char *)Self->DataItems[index].Data);
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

                  GlobalUnlock(stgm.hGlobal);
               }
               else error = ERR_Lock;

               ReleaseStgMedium(&stgm);
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

      if (!(glDropTarget = HeapAlloc(glHeap, 0, sizeof(RK_IDROPTARGET)))) return ERR_Failed;
      glDropTarget->idt.lpVtbl   = (void *)&idt_vtbl;
      glDropTarget->lRefCount    = 1;
      glDropTarget->tb_pDragFile = NULL;
   }

   if (RegisterDragDrop(Window, &glDropTarget->idt) != S_OK) {
      MSG("RegisterDragDrop() failed.\n");
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
         else error = ERR_Lock;
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
         else error = ERR_Lock;
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
}
